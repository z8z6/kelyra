#include "Driver/DirectDxil.h"
#include "Driver/DxilValidator.h"

#include "IR/Kelyra.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/Frontend/HLSL/SemanticSignatures.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsDirectX.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include <map>
#include <memory>
#include <stdexcept>

namespace kelyra::driver {
namespace {

using namespace llvm;

StringRef Name(mlir::Operation &Op) { return Op.getName().getStringRef(); }

StringRef String(mlir::Operation &Op, StringRef Key) {
  return mlir::cast<mlir::StringAttr>(Op.getAttr(Key)).getValue();
}

class Lowerer {
  mlir::ModuleOp ShaderModule;
  LLVMContext Context;
  std::unique_ptr<Module> Output;
  IRBuilder<> Builder;
  std::map<std::string, StructType *> Records;
  std::map<std::string, mlir::Operation *> RecordOps;
  std::map<std::string, Function *> Functions;
  DenseMap<mlir::Value, Value *> Values;
  Function *Current = nullptr;
  mlir::Operation *Entry = nullptr;
  bool Vertex = false;

  Type *TypeOf(mlir::Type Ty) {
    if (Ty.isF32())
      return Builder.getFloatTy();
    if (auto Int = mlir::dyn_cast<mlir::IntegerType>(Ty))
      return Builder.getIntNTy(Int.getWidth());
    if (auto Record = mlir::dyn_cast<ir::ShaderRecordType>(Ty)) {
      const auto It = Records.find(Record.getName().str());
      if (It == Records.end())
        throw std::runtime_error("unknown shader record");
      return It->second;
    }
    if (mlir::isa<ir::ShaderResourceType>(Ty))
      throw std::runtime_error(
          "DXIL texture and sampler resource lowering is not implemented");
    throw std::runtime_error("unsupported Shader IR type for DXIL");
  }

  static mlir::ArrayAttr Array(mlir::Operation &Op, StringRef Key) {
    return mlir::cast<mlir::ArrayAttr>(Op.getAttr(Key));
  }

  Value *Get(mlir::Value V) {
    const auto It = Values.find(V);
    if (It == Values.end())
      throw std::runtime_error("Shader IR value has no DXIL mapping");
    return It->second;
  }

  unsigned FieldIndex(mlir::Type RecordType, StringRef Field) {
    auto Record = mlir::cast<ir::ShaderRecordType>(RecordType);
    auto *Op = RecordOps.at(Record.getName().str());
    auto Fields = Array(*Op, "fields");
    for (unsigned I = 0; I < Fields.size(); ++I)
      if (mlir::cast<mlir::StringAttr>(Fields[I]).getValue() == Field)
        return I;
    throw std::runtime_error("unknown Shader IR record field");
  }

  Value *StorePath(Value *Record, mlir::Type RecordType, mlir::ArrayAttr Path,
                   unsigned At, Value *NewValue) {
    const unsigned Index = FieldIndex(
        RecordType, mlir::cast<mlir::StringAttr>(Path[At]).getValue());
    if (At + 1 == Path.size())
      return Builder.CreateInsertValue(Record, NewValue, {Index});
    auto *RecordOp = RecordOps.at(
        mlir::cast<ir::ShaderRecordType>(RecordType).getName().str());
    mlir::Type ChildType =
        mlir::cast<mlir::TypeAttr>(Array(*RecordOp, "field_types")[Index])
            .getValue();
    Value *Child = Builder.CreateExtractValue(Record, {Index});
    Child = StorePath(Child, ChildType, Path, At + 1, NewValue);
    return Builder.CreateInsertValue(Record, Child, {Index});
  }

  bool HasTerminator() const {
    const auto *Block = Builder.GetInsertBlock();
    return !Block->empty() && Block->back().isTerminator();
  }

  void LowerBlock(mlir::Block &Block) {
    for (mlir::Operation &Op : Block) {
      if (HasTerminator())
        break;
      StringRef Kind = Name(Op);
      Value *Result = nullptr;
      if (Kind == "kelyra.shader.literal") {
        Type *Ty = TypeOf(Op.getResult(0).getType());
        StringRef Text = String(Op, "text");
        if (Ty->isFloatingPointTy()) {
          double Number = 0;
          if (Text.getAsDouble(Number))
            throw std::runtime_error("invalid shader float literal");
          Result = ConstantFP::get(Ty, Number);
        } else if (Text == "true" || Text == "false") {
          Result = ConstantInt::get(Ty, Text == "true");
        } else {
          uint64_t Number = 0;
          if (Text.getAsInteger(0, Number))
            throw std::runtime_error("invalid shader integer literal");
          Result = ConstantInt::get(Ty, Number);
        }
      } else if (Kind == "kelyra.shader.var") {
        Type *Ty =
            TypeOf(mlir::cast<ir::ShaderRefType>(Op.getResult(0).getType())
                       .getValueType());
        auto *Slot = Builder.CreateAlloca(Ty);
        if (Op.getNumOperands())
          Builder.CreateStore(Get(Op.getOperand(0)), Slot);
        Result = Slot;
      } else if (Kind == "kelyra.shader.load") {
        Result = Builder.CreateLoad(TypeOf(Op.getResult(0).getType()),
                                    Get(Op.getOperand(0)));
      } else if (Kind == "kelyra.shader.store") {
        Builder.CreateStore(Get(Op.getOperand(1)), Get(Op.getOperand(0)));
      } else if (Kind == "kelyra.shader.store_field") {
        auto Ref = mlir::cast<ir::ShaderRefType>(Op.getOperand(0).getType());
        Value *Pointer = Get(Op.getOperand(0));
        Value *Old = Builder.CreateLoad(TypeOf(Ref.getValueType()), Pointer);
        Value *New = StorePath(Old, Ref.getValueType(), Array(Op, "path"), 0,
                               Get(Op.getOperand(1)));
        Builder.CreateStore(New, Pointer);
      } else if (Kind == "kelyra.shader.unary") {
        Value *Input = Get(Op.getOperand(0));
        StringRef Code = String(Op, "opcode");
        if (Code == "-")
          Result = Input->getType()->isFloatingPointTy()
                       ? Builder.CreateFNeg(Input)
                       : Builder.CreateNeg(Input);
        else if (Code == "!")
          Result = Builder.CreateNot(Input);
        else
          throw std::runtime_error("unsupported shader unary operation");
      } else if (Kind == "kelyra.shader.binary") {
        Value *L = Get(Op.getOperand(0));
        Value *R = Get(Op.getOperand(1));
        StringRef Code = String(Op, "opcode");
        bool Float = L->getType()->isFloatingPointTy();
        bool Unsigned =
            mlir::isa<mlir::IntegerType>(Op.getOperand(0).getType()) &&
            mlir::cast<mlir::IntegerType>(Op.getOperand(0).getType())
                .isUnsigned();
        if (Code == "+")
          Result = Float ? Builder.CreateFAdd(L, R) : Builder.CreateAdd(L, R);
        else if (Code == "-")
          Result = Float ? Builder.CreateFSub(L, R) : Builder.CreateSub(L, R);
        else if (Code == "*")
          Result = Float ? Builder.CreateFMul(L, R) : Builder.CreateMul(L, R);
        else if (Code == "/")
          Result = Float      ? Builder.CreateFDiv(L, R)
                   : Unsigned ? Builder.CreateUDiv(L, R)
                              : Builder.CreateSDiv(L, R);
        else if (Code == "==")
          Result =
              Float ? Builder.CreateFCmpOEQ(L, R) : Builder.CreateICmpEQ(L, R);
        else if (Code == "!=")
          Result =
              Float ? Builder.CreateFCmpONE(L, R) : Builder.CreateICmpNE(L, R);
        else if (Code == "<")
          Result = Float      ? Builder.CreateFCmpOLT(L, R)
                   : Unsigned ? Builder.CreateICmpULT(L, R)
                              : Builder.CreateICmpSLT(L, R);
        else if (Code == ">")
          Result = Float      ? Builder.CreateFCmpOGT(L, R)
                   : Unsigned ? Builder.CreateICmpUGT(L, R)
                              : Builder.CreateICmpSGT(L, R);
        else if (Code == "<=")
          Result = Float      ? Builder.CreateFCmpOLE(L, R)
                   : Unsigned ? Builder.CreateICmpULE(L, R)
                              : Builder.CreateICmpSLE(L, R);
        else if (Code == ">=")
          Result = Float      ? Builder.CreateFCmpOGE(L, R)
                   : Unsigned ? Builder.CreateICmpUGE(L, R)
                              : Builder.CreateICmpSGE(L, R);
        else if (Code == "&&")
          Result = Builder.CreateAnd(L, R);
        else if (Code == "||")
          Result = Builder.CreateOr(L, R);
        else
          throw std::runtime_error("unsupported shader binary operation");
      } else if (Kind == "kelyra.shader.cast") {
        Value *Input = Get(Op.getOperand(0));
        Type *Target = TypeOf(Op.getResult(0).getType());
        if (Input->getType() == Target)
          Result = Input;
        else if (Target->isFloatingPointTy())
          Result = Builder.CreateUIToFP(Input, Target);
        else if (Input->getType()->isFloatingPointTy())
          Result = Builder.CreateFPToUI(Input, Target);
        else
          Result = Builder.CreateIntCast(Input, Target, false);
      } else if (Kind == "kelyra.shader.construct") {
        Type *Ty = TypeOf(Op.getResult(0).getType());
        Result = UndefValue::get(Ty);
        for (unsigned I = 0; I < Op.getNumOperands(); ++I)
          Result =
              Builder.CreateInsertValue(Result, Get(Op.getOperand(I)), {I});
      } else if (Kind == "kelyra.shader.extract") {
        unsigned Index =
            FieldIndex(Op.getOperand(0).getType(), String(Op, "field"));
        Result = Builder.CreateExtractValue(Get(Op.getOperand(0)), {Index});
      } else if (Kind == "kelyra.shader.call") {
        StringRef Callee =
            mlir::cast<mlir::FlatSymbolRefAttr>(Op.getAttr("callee"))
                .getValue();
        SmallVector<Value *> Args;
        for (mlir::Value Arg : Op.getOperands())
          Args.push_back(Get(Arg));
        Result = Builder.CreateCall(Functions.at(Callee.str()), Args);
      } else if (Kind == "kelyra.shader.intrinsic") {
        StringRef IntrinsicName = String(Op, "name");
        Intrinsic::ID Id = IntrinsicName == "sqrt"  ? Intrinsic::sqrt
                           : IntrinsicName == "pow" ? Intrinsic::pow
                                                    : Intrinsic::not_intrinsic;
        if (Id == Intrinsic::not_intrinsic)
          throw std::runtime_error("unsupported DXIL shader intrinsic");
        SmallVector<Value *> Args;
        for (mlir::Value Arg : Op.getOperands())
          Args.push_back(Get(Arg));
        Function *Fn = Intrinsic::getOrInsertDeclaration(
            Output.get(), Id, {TypeOf(Op.getResult(0).getType())});
        Result = Builder.CreateCall(Fn, Args);
      } else if (Kind == "kelyra.shader.if") {
        auto *Then = BasicBlock::Create(Context, "then", Current);
        auto *Else = BasicBlock::Create(Context, "else", Current);
        auto *Merge = BasicBlock::Create(Context, "merge", Current);
        Builder.CreateCondBr(Get(Op.getOperand(0)), Then, Else);
        Builder.SetInsertPoint(Then);
        LowerBlock(Op.getRegion(0).front());
        if (!HasTerminator())
          Builder.CreateBr(Merge);
        Builder.SetInsertPoint(Else);
        LowerBlock(Op.getRegion(1).front());
        if (!HasTerminator())
          Builder.CreateBr(Merge);
        Builder.SetInsertPoint(Merge);
      } else if (Kind == "kelyra.shader.return") {
        Builder.CreateRet(Get(Op.getOperand(0)));
      } else if (Kind == "kelyra.shader.yield" ||
                 Kind == "kelyra.shader.eval") {
      } else {
        throw std::runtime_error(
            ("unsupported Shader IR op in DXIL lowering: " + Kind).str());
      }
      if (Result && Op.getNumResults())
        Values[Op.getResult(0)] = Result;
    }
  }

  void MakeRecords() {
    for (mlir::Operation &Op : *ShaderModule.getBody())
      if (Name(Op) == "kelyra.shader.record") {
        std::string Symbol = String(Op, "sym_name").str();
        Records[Symbol] = StructType::create(Context, Symbol);
        RecordOps[Symbol] = &Op;
      }
    for (const auto &[Symbol, Record] : Records) {
      SmallVector<Type *> Members;
      for (mlir::Attribute Attr : Array(*RecordOps.at(Symbol), "field_types"))
        Members.push_back(TypeOf(mlir::cast<mlir::TypeAttr>(Attr).getValue()));
      Record->setBody(Members);
    }
  }

  void MakeFunctions() {
    for (mlir::Operation &Op : *ShaderModule.getBody()) {
      if (Name(Op) != "kelyra.shader.func")
        continue;
      auto Signature = mlir::cast<mlir::FunctionType>(
          mlir::cast<mlir::TypeAttr>(Op.getAttr("function_type")).getValue());
      SmallVector<Type *> Args;
      for (mlir::Type Arg : Signature.getInputs())
        Args.push_back(TypeOf(Arg));
      Type *Return = TypeOf(Signature.getResult(0));
      std::string Symbol = String(Op, "sym_name").str();
      Functions[Symbol] =
          Function::Create(FunctionType::get(Return, Args, false),
                           GlobalValue::InternalLinkage, Symbol, Output.get());
      if (String(Op, "stage") != "helper")
        Entry = &Op;
    }
    if (!Entry)
      throw std::runtime_error("Shader IR contains no entry function");
    Vertex = String(*Entry, "stage") == "vertex";
    if (!Vertex && String(*Entry, "stage") != "fragment")
      throw std::runtime_error("unsupported DXIL shader stage");
    for (mlir::Operation &Op : *ShaderModule.getBody()) {
      if (Name(Op) != "kelyra.shader.func")
        continue;
      Current = Functions.at(String(Op, "sym_name").str());
      BasicBlock *Block = BasicBlock::Create(Context, "entry", Current);
      Builder.SetInsertPoint(Block);
      Values.clear();
      auto Arg = Current->arg_begin();
      for (mlir::Value Param : Op.getRegion(0).front().getArguments())
        Values[Param] = &*Arg++;
      LowerBlock(Op.getRegion(0).front());
      if (!HasTerminator())
        Builder.CreateUnreachable();
    }
  }

  void AddSignature(Function *Main,
                    ArrayRef<hlsl::SemanticSignatureElement> Inputs,
                    ArrayRef<hlsl::SemanticSignatureElement> Outputs) {
    SmallVector<Metadata *> InNodes, OutNodes;
    for (const auto &Element : Inputs)
      InNodes.push_back(Element.toMetadata(Context));
    for (const auto &Element : Outputs)
      OutNodes.push_back(Element.toMetadata(Context));
    auto *Triple = MDNode::get(Context, {ValueAsMetadata::get(Main),
                                         MDNode::get(Context, InNodes),
                                         MDNode::get(Context, OutNodes)});
    Output->getOrInsertNamedMetadata("dx.semantic.signatures")
        ->addOperand(Triple);
    Output->getOrInsertNamedMetadata("dx.valver")
        ->addOperand(MDNode::get(
            Context, {ConstantAsMetadata::get(Builder.getInt32(1)),
                      ConstantAsMetadata::get(Builder.getInt32(8))}));
  }

  void MakeEntry() {
    Current =
        Function::Create(FunctionType::get(Builder.getVoidTy(), false),
                         GlobalValue::ExternalLinkage, "main", Output.get());
    Current->addFnAttr("hlsl.shader", Vertex ? "vertex" : "pixel");
    Current->addFnAttr(Attribute::Convergent);
    Current->addFnAttr(Attribute::NoInline);
    Builder.SetInsertPoint(BasicBlock::Create(Context, "entry", Current));
    SmallVector<Value *> Args;
    SmallVector<hlsl::SemanticSignatureElement> Inputs, Outputs;
    auto Signature = mlir::cast<mlir::FunctionType>(
        mlir::cast<mlir::TypeAttr>(Entry->getAttr("function_type")).getValue());
    auto Decorations = Array(*Entry, "input_decorations");
    for (unsigned I = 0; I < Signature.getNumInputs(); ++I) {
      auto Item = mlir::cast<mlir::ArrayAttr>(Decorations[I]);
      StringRef Semantic;
      unsigned SemanticIndex = 0;
      for (mlir::Attribute Attr : Item) {
        StringRef Text = mlir::cast<mlir::StringAttr>(Attr).getValue();
        if (Text == "vertex_index")
          Semantic = "SV_VertexID";
        else if (Text.starts_with("location:")) {
          Semantic = "TEXCOORD";
          if (Text.drop_front(9).getAsInteger(10, SemanticIndex))
            throw std::runtime_error("invalid location decoration");
        }
      }
      if (Semantic.empty())
        throw std::runtime_error("unsupported DXIL shader input");
      Type *Ty = TypeOf(Signature.getInput(I));
      auto Component =
          Ty->isFloatTy() ? dxil::ElementType::F32
          : mlir::cast<mlir::IntegerType>(Signature.getInput(I)).isUnsigned()
              ? dxil::ElementType::U32
              : dxil::ElementType::I32;
      Inputs.emplace_back(I, Semantic, Component,
                          hlsl::getSemanticKind(Semantic),
                          ArrayRef<uint32_t>(SemanticIndex), 1);
      Inputs.back().UsageMask = 1;
      Function *Load = Intrinsic::getOrInsertDeclaration(
          Output.get(), Intrinsic::dx_load_input, {Ty});
      Args.push_back(Builder.CreateCall(
          Load, {Builder.getInt32(I), Builder.getInt32(0), Builder.getInt8(0),
                 PoisonValue::get(Builder.getInt32Ty())}));
    }
    Value *Result = Builder.CreateCall(
        Functions.at(String(*Entry, "sym_name").str()), Args);
    auto ReturnRecord =
        mlir::cast<ir::ShaderRecordType>(Signature.getResult(0));
    auto *Record = RecordOps.at(ReturnRecord.getName().str());
    auto Fields = Array(*Record, "fields");
    auto FieldTypes = Array(*Record, "field_types");
    auto OutputDecorations = Array(*Record, "decorations");
    for (unsigned I = 0; I < Fields.size(); ++I) {
      StringRef Semantic;
      unsigned SemanticIndex = 0;
      for (mlir::Attribute Attr :
           mlir::cast<mlir::ArrayAttr>(OutputDecorations[I])) {
        StringRef Text = mlir::cast<mlir::StringAttr>(Attr).getValue();
        if (Text == "position")
          Semantic = "SV_Position";
        else if (Text.starts_with("location:")) {
          Semantic = "SV_Target";
          if (Text.drop_front(9).getAsInteger(10, SemanticIndex))
            throw std::runtime_error("invalid output location");
        }
      }
      if (Semantic.empty())
        throw std::runtime_error("unsupported DXIL shader output");
      mlir::Type FieldType =
          mlir::cast<mlir::TypeAttr>(FieldTypes[I]).getValue();
      auto Vec = mlir::dyn_cast<ir::ShaderRecordType>(FieldType);
      if (!Vec)
        throw std::runtime_error("DXIL output needs vector record");
      auto *VecRecord = RecordOps.at(Vec.getName().str());
      unsigned Count = Array(*VecRecord, "fields").size();
      Outputs.emplace_back(I, Semantic, dxil::ElementType::F32,
                           hlsl::getSemanticKind(Semantic),
                           ArrayRef<uint32_t>(SemanticIndex), Count);
      Outputs.back().UsageMask = (1U << Count) - 1U;
      if (Semantic == "SV_Position")
        Outputs.back().InterpMode =
            dxbc::PSV::InterpolationMode::LinearNoperspective;
      Value *Field = Builder.CreateExtractValue(Result, {I});
      Function *Store = Intrinsic::getOrInsertDeclaration(
          Output.get(), Intrinsic::dx_store_output, {Builder.getFloatTy()});
      for (unsigned Component = 0; Component < Count; ++Component) {
        Value *Scalar = Builder.CreateExtractValue(Field, {Component});
        Builder.CreateCall(Store, {Builder.getInt32(I), Builder.getInt32(0),
                                   Builder.getInt8(Component), Scalar});
      }
    }
    Builder.CreateRetVoid();
    AddSignature(Current, Inputs, Outputs);
  }

public:
  explicit Lowerer(mlir::ModuleOp Module)
      : ShaderModule(Module),
        Output(std::make_unique<llvm::Module>("kelyra-shader", Context)),
        Builder(Context) {}

  bool Run(StringRef Path, std::string &Error) {
    try {
      MakeRecords();
      MakeFunctions();
      MakeEntry();
      llvm::Triple Triple(Vertex ? "dxilv1.0-pc-shadermodel6.0-vertex"
                                 : "dxilv1.0-pc-shadermodel6.0-pixel");
      LLVMInitializeDirectXTargetInfo();
      LLVMInitializeDirectXTarget();
      LLVMInitializeDirectXTargetMC();
      LLVMInitializeDirectXAsmPrinter();
      std::string TargetError;
      const Target *DxTarget =
          TargetRegistry::lookupTarget(Triple, TargetError);
      if (!DxTarget)
        throw std::runtime_error(TargetError);
      TargetOptions Options;
      std::unique_ptr<TargetMachine> Machine(DxTarget->createTargetMachine(
          Triple, "generic", "", Options, Reloc::Static, std::nullopt,
          CodeGenOptLevel::Default));
      if (!Machine)
        throw std::runtime_error("DirectX target machine unavailable");
      Output->setTargetTriple(Triple);
      Output->setDataLayout(Machine->createDataLayout());
      LoopAnalysisManager LAM;
      FunctionAnalysisManager FAM;
      CGSCCAnalysisManager CGAM;
      ModuleAnalysisManager MAM;
      PassBuilder Pipeline(Machine.get());
      Pipeline.registerModuleAnalyses(MAM);
      Pipeline.registerCGSCCAnalyses(CGAM);
      Pipeline.registerFunctionAnalyses(FAM);
      Pipeline.registerLoopAnalyses(LAM);
      Pipeline.crossRegisterProxies(LAM, FAM, CGAM, MAM);
      ModulePassManager Optimizer =
          Pipeline.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
      Optimizer.run(*Output, MAM);
      std::string VerifyError;
      raw_string_ostream VerifyStream(VerifyError);
      if (verifyModule(*Output, &VerifyStream))
        throw std::runtime_error(VerifyStream.str());
      std::error_code EC;
      ToolOutputFile File(Path, EC, sys::fs::OF_None);
      if (EC)
        throw std::runtime_error(EC.message());
      legacy::PassManager Passes;
      if (Machine->addPassesToEmitFile(Passes, File.os(), nullptr,
                                       CodeGenFileType::ObjectFile))
        throw std::runtime_error("DirectX backend cannot emit DXContainer");
      Passes.run(*Output);
      File.os().flush();
      if (File.os().has_error())
        throw std::runtime_error("writing DXContainer failed");
      File.keep();
      if (!ValidateAndSignDxil(Path, Error))
        return false;
      return true;
    } catch (const std::exception &Exception) {
      Error = Exception.what();
      return false;
    }
  }
};

} // namespace

bool EmitDirectDxil(mlir::ModuleOp ShaderModule, llvm::StringRef OutputPath,
                    std::string &Error) {
  return Lowerer(ShaderModule).Run(OutputPath, Error);
}

} // namespace kelyra::driver
