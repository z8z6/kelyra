#include "Driver/DirectSpirv.h"

#include "IR/Kelyra.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Target/SPIRV/Serialization.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mlir;

namespace {

std::string AttrString(Operation *Op, llvm::StringRef Name) {
  auto Attr = Op->getAttrOfType<StringAttr>(Name);
  if (!Attr)
    throw std::runtime_error("missing shader attribute: " + Name.str());
  return Attr.getValue().str();
}

struct RecordInfo {
  std::vector<std::string> Fields;
  std::vector<Type> FieldTypes;
  std::vector<std::vector<std::string>> Decorations;
  bool Vector = false;
};

class SpirvLowering {
  ModuleOp Input;
  std::map<std::string, RecordInfo> Records;
  std::map<std::string, Operation *> Functions;
  Operation *Entry = nullptr;
  std::ostringstream Source;
  unsigned NextValue = 0;
  unsigned NextBlock = 0;
  llvm::DenseMap<Value, std::string> Values;
  std::ostringstream Prologue;
  std::ostringstream Body;
  struct LoopTargets {
    std::string Continue;
    std::string Merge;
  };
  std::vector<LoopTargets> Loops;

  std::string NewValue() { return "%v" + std::to_string(NextValue++); }

  std::string ValueName(Value Value) const {
    const auto It = Values.find(Value);
    if (It == Values.end())
      throw std::runtime_error(
          "SPIR-V lowering found an unmapped shader value");
    return It->second;
  }

  const RecordInfo &Record(Type Ty) const {
    auto Named = dyn_cast<kelyra::ir::ShaderRecordType>(Ty);
    if (!Named)
      throw std::runtime_error("expected shader data record");
    return Records.at(Named.getName().getValue().str());
  }

  std::string TypeName(Type Ty) const {
    if (Ty.isF32())
      return "f32";
    if (Ty.isInteger(1))
      return "i1";
    if (Ty.isInteger(32))
      return "i32";
    if (auto Ref = dyn_cast<kelyra::ir::ShaderRefType>(Ty))
      return "!spirv.ptr<" + TypeName(Ref.getValueType()) + ", Function>";
    if (auto Resource = dyn_cast<kelyra::ir::ShaderResourceType>(Ty)) {
      if (Resource.getKind().getValue() == "texture2d")
        return "!spirv.image<f32, Dim2D, NoDepth, NonArrayed, "
               "SingleSampled, NeedSampler, Unknown>";
      if (Resource.getKind().getValue() == "sampler")
        return "!spirv.sampler";
      throw std::runtime_error("unsupported SPIR-V shader resource kind");
    }
    if (auto Named = dyn_cast<kelyra::ir::ShaderRecordType>(Ty)) {
      const auto &Info = Records.at(Named.getName().getValue().str());
      if (Info.Vector)
        return "vector<" + std::to_string(Info.Fields.size()) + "x" +
               TypeName(Info.FieldTypes.front()) + ">";
      std::string Text = "!spirv.struct<( ";
      for (size_t I = 0; I < Info.FieldTypes.size(); ++I) {
        if (I)
          Text += ", ";
        Text += TypeName(Info.FieldTypes[I]);
      }
      return Text + " )>";
    }
    throw std::runtime_error("shader type cannot lower to SPIR-V");
  }

  unsigned FieldIndex(Type Ty, llvm::StringRef Field) const {
    const auto &Info = Record(Ty);
    auto Found = std::find(Info.Fields.begin(), Info.Fields.end(), Field);
    if (Found == Info.Fields.end())
      throw std::runtime_error("unknown shader record field: " + Field.str());
    return static_cast<unsigned>(Found - Info.Fields.begin());
  }

  Type FieldType(Type Ty, unsigned Index) const {
    return Record(Ty).FieldTypes.at(Index);
  }

  std::string EmitUnary(llvm::StringRef Name, const std::string &InputValue,
                        Type Ty) {
    auto Out = NewValue();
    Body << "    " << Out << " = " << Name.str() << " " << InputValue << " : "
         << TypeName(Ty) << "\n";
    return Out;
  }

  std::string EmitBinary(llvm::StringRef Name, const std::string &Lhs,
                         const std::string &Rhs, Type OperandTy) {
    auto Out = NewValue();
    Body << "    " << Out << " = " << Name.str() << " " << Lhs << ", " << Rhs
         << " : " << TypeName(OperandTy) << "\n";
    return Out;
  }

  std::string Extract(const std::string &Composite, Type CompositeTy,
                      unsigned Index) {
    auto Out = NewValue();
    Body << "    " << Out << " = spirv.CompositeExtract " << Composite << "["
         << Index << " : i32] : " << TypeName(CompositeTy) << "\n";
    return Out;
  }

  std::string Insert(const std::string &Composite, Type CompositeTy,
                     const std::string &Element, Type ElementTy,
                     unsigned Index) {
    auto Out = NewValue();
    Body << "    " << Out << " = spirv.CompositeInsert " << Element << ", "
         << Composite << "[" << Index << " : i32] : " << TypeName(ElementTy)
         << " into " << TypeName(CompositeTy) << "\n";
    return Out;
  }

  std::string InsertPath(const std::string &Composite, Type CompositeTy,
                         const std::string &Element, ArrayRef<Attribute> Path,
                         size_t Depth = 0) {
    auto Field = cast<StringAttr>(Path[Depth]).getValue();
    auto Index = FieldIndex(CompositeTy, Field);
    auto ElementTy = FieldType(CompositeTy, Index);
    if (Depth + 1 == Path.size())
      return Insert(Composite, CompositeTy, Element, ElementTy, Index);
    auto Child = Extract(Composite, CompositeTy, Index);
    auto Updated = InsertPath(Child, ElementTy, Element, Path, Depth + 1);
    return Insert(Composite, CompositeTy, Updated, ElementTy, Index);
  }

  static std::string BinaryName(llvm::StringRef Op, Type Ty) {
    const bool Float = Ty.isF32();
    const bool Unsigned =
        isa<IntegerType>(Ty) && cast<IntegerType>(Ty).isUnsigned();
    if (Op == "+")
      return Float ? "spirv.FAdd" : "spirv.IAdd";
    if (Op == "-")
      return Float ? "spirv.FSub" : "spirv.ISub";
    if (Op == "*")
      return Float ? "spirv.FMul" : "spirv.IMul";
    if (Op == "/")
      return Float ? "spirv.FDiv" : Unsigned ? "spirv.UDiv" : "spirv.SDiv";
    if (Op == "%")
      return Float ? "spirv.FRem" : Unsigned ? "spirv.UMod" : "spirv.SRem";
    if (Op == "==")
      return Float ? "spirv.FOrdEqual" : "spirv.IEqual";
    if (Op == "!=")
      return Float ? "spirv.FOrdNotEqual" : "spirv.INotEqual";
    if (Op == "<")
      return Float      ? "spirv.FOrdLessThan"
             : Unsigned ? "spirv.ULessThan"
                        : "spirv.SLessThan";
    if (Op == "<=")
      return Float      ? "spirv.FOrdLessThanEqual"
             : Unsigned ? "spirv.ULessThanEqual"
                        : "spirv.SLessThanEqual";
    if (Op == ">")
      return Float      ? "spirv.FOrdGreaterThan"
             : Unsigned ? "spirv.UGreaterThan"
                        : "spirv.SGreaterThan";
    if (Op == ">=")
      return Float      ? "spirv.FOrdGreaterThanEqual"
             : Unsigned ? "spirv.UGreaterThanEqual"
                        : "spirv.SGreaterThanEqual";
    if (Op == "&&")
      return "spirv.LogicalAnd";
    if (Op == "||")
      return "spirv.LogicalOr";
    throw std::runtime_error("unsupported shader binary operator: " + Op.str());
  }

  void LowerBlock(Block &Block) {
    for (Operation &Op : Block) {
      auto Name = Op.getName().getStringRef();
      if (Name == "kelyra.shader.yield")
        continue;
      if (Name == "kelyra.shader.literal") {
        auto Ty = TypeName(Op.getResult(0).getType());
        auto Text = AttrString(&Op, "text");
        if (Ty == "f32" && Text.find_first_of(".eE") == std::string::npos)
          Text += ".0";
        auto Out = NewValue();
        Body << "    " << Out << " = spirv.Constant " << Text;
        if (Ty != "i1")
          Body << " : " << Ty;
        Body << "\n";
        Values[Op.getResult(0)] = Out;
      } else if (Name == "kelyra.shader.var") {
        auto ValueType =
            cast<kelyra::ir::ShaderRefType>(Op.getResult(0).getType())
                .getValueType();
        if (isa<kelyra::ir::ShaderResourceType>(ValueType)) {
          if (Op.getNumOperands() != 1)
            throw std::runtime_error("shader resource cannot be constructed");
          Values[Op.getResult(0)] = ValueName(Op.getOperand(0));
          continue;
        }
        auto Out = NewValue();
        auto Ty = cast<kelyra::ir::ShaderRefType>(Op.getResult(0).getType())
                      .getValueType();
        Prologue << "    " << Out << " = spirv.Variable : !spirv.ptr<"
                 << TypeName(Ty) << ", Function>\n";
        Values[Op.getResult(0)] = Out;
        if (Op.getNumOperands())
          Body << "    spirv.Store \"Function\" " << Out << ", "
               << ValueName(Op.getOperand(0)) << " : " << TypeName(Ty) << "\n";
      } else if (Name == "kelyra.shader.load") {
        if (isa<kelyra::ir::ShaderResourceType>(Op.getResult(0).getType())) {
          Values[Op.getResult(0)] = ValueName(Op.getOperand(0));
          continue;
        }
        auto Out = NewValue();
        Body << "    " << Out << " = spirv.Load \"Function\" "
             << ValueName(Op.getOperand(0)) << " : "
             << TypeName(Op.getResult(0).getType()) << "\n";
        Values[Op.getResult(0)] = Out;
      } else if (Name == "kelyra.shader.store") {
        Body << "    spirv.Store \"Function\" " << ValueName(Op.getOperand(0))
             << ", " << ValueName(Op.getOperand(1)) << " : "
             << TypeName(Op.getOperand(1).getType()) << "\n";
      } else if (Name == "kelyra.shader.store_field") {
        auto Ref = Op.getOperand(0);
        auto Ty = cast<kelyra::ir::ShaderRefType>(Ref.getType()).getValueType();
        auto Loaded = NewValue();
        Body << "    " << Loaded << " = spirv.Load \"Function\" "
             << ValueName(Ref) << " : " << TypeName(Ty) << "\n";
        auto Path = Op.getAttrOfType<ArrayAttr>("path");
        auto Updated = InsertPath(Loaded, Ty, ValueName(Op.getOperand(1)),
                                  Path.getValue());
        Body << "    spirv.Store \"Function\" " << ValueName(Ref) << ", "
             << Updated << " : " << TypeName(Ty) << "\n";
      } else if (Name == "kelyra.shader.unary") {
        auto Opcode = AttrString(&Op, "opcode");
        auto In = Op.getOperand(0);
        auto Lowered = Opcode == "!"          ? "spirv.LogicalNot"
                       : In.getType().isF32() ? "spirv.FNegate"
                                              : "spirv.SNegate";
        Values[Op.getResult(0)] =
            EmitUnary(Lowered, ValueName(In), In.getType());
      } else if (Name == "kelyra.shader.binary") {
        auto Lhs = Op.getOperand(0);
        Values[Op.getResult(0)] = EmitBinary(
            BinaryName(AttrString(&Op, "opcode"), Lhs.getType()),
            ValueName(Lhs), ValueName(Op.getOperand(1)), Lhs.getType());
      } else if (Name == "kelyra.shader.cast") {
        auto From = Op.getOperand(0).getType();
        auto To = Op.getResult(0).getType();
        auto In = ValueName(Op.getOperand(0));
        if (TypeName(From) == TypeName(To)) {
          Values[Op.getResult(0)] = In;
          continue;
        }
        std::string OpName;
        if (From.isF32() && To.isInteger(32))
          OpName = cast<IntegerType>(To).isUnsigned() ? "spirv.ConvertFToU"
                                                      : "spirv.ConvertFToS";
        else if (From.isInteger(32) && To.isF32())
          OpName = cast<IntegerType>(From).isUnsigned() ? "spirv.ConvertUToF"
                                                        : "spirv.ConvertSToF";
        else
          throw std::runtime_error("unsupported shader SPIR-V cast");
        auto Out = NewValue();
        Body << "    " << Out << " = " << OpName << " " << In << " : "
             << TypeName(From) << " to " << TypeName(To) << "\n";
        Values[Op.getResult(0)] = Out;
      } else if (Name == "kelyra.shader.construct") {
        auto Out = NewValue();
        Body << "    " << Out << " = spirv.CompositeConstruct ";
        for (unsigned I = 0; I < Op.getNumOperands(); ++I) {
          if (I)
            Body << ", ";
          Body << ValueName(Op.getOperand(I));
        }
        Body << " : (";
        for (unsigned I = 0; I < Op.getNumOperands(); ++I) {
          if (I)
            Body << ", ";
          Body << TypeName(Op.getOperand(I).getType());
        }
        Body << ") -> " << TypeName(Op.getResult(0).getType()) << "\n";
        Values[Op.getResult(0)] = Out;
      } else if (Name == "kelyra.shader.extract") {
        auto RecordValue = Op.getOperand(0);
        Values[Op.getResult(0)] = Extract(
            ValueName(RecordValue), RecordValue.getType(),
            FieldIndex(RecordValue.getType(), AttrString(&Op, "field")));
      } else if (Name == "kelyra.shader.call") {
        auto Callee = Op.getAttrOfType<FlatSymbolRefAttr>("callee");
        auto Out = NewValue();
        Body << "    " << Out << " = spirv.FunctionCall @"
             << Callee.getValue().str() << "(";
        for (unsigned I = 0; I < Op.getNumOperands(); ++I) {
          if (I)
            Body << ", ";
          Body << ValueName(Op.getOperand(I));
        }
        Body << ") : (";
        for (unsigned I = 0; I < Op.getNumOperands(); ++I) {
          if (I)
            Body << ", ";
          Body << TypeName(Op.getOperand(I).getType());
        }
        Body << ") -> " << TypeName(Op.getResult(0).getType()) << "\n";
        Values[Op.getResult(0)] = Out;
      } else if (Name == "kelyra.shader.intrinsic") {
        auto Intrinsic = AttrString(&Op, "name");
        if (Intrinsic == "sample_2d") {
          if (Op.getNumOperands() != 3 ||
              TypeName(Op.getOperand(0).getType()).find("!spirv.image<") != 0 ||
              TypeName(Op.getOperand(1).getType()) != "!spirv.sampler" ||
              TypeName(Op.getOperand(2).getType()) != "vector<2xf32>" ||
              TypeName(Op.getResult(0).getType()) != "vector<4xf32>")
            throw std::runtime_error("sample_2d expects Texture2D, "
                                     "SamplerState, Vec2 and returns Vec4");
          const auto ImageType = TypeName(Op.getOperand(0).getType());
          const auto SampledType = "!spirv.sampled_image<" + ImageType + ">";
          const auto Sampled = NewValue();
          Body << "    " << Sampled << " = spirv.SampledImage "
               << ValueName(Op.getOperand(0)) << ", "
               << ValueName(Op.getOperand(1)) << " : " << ImageType
               << ", !spirv.sampler -> " << SampledType << "\n";
          const auto Out = NewValue();
          Body << "    " << Out << " = spirv.ImageSampleImplicitLod " << Sampled
               << ", " << ValueName(Op.getOperand(2)) << " : " << SampledType
               << ", vector<2xf32> -> vector<4xf32>\n";
          Values[Op.getResult(0)] = Out;
          continue;
        }
        static const std::map<std::string, std::string> Intrinsics{
            {"sqrt", "spirv.GL.Sqrt"}, {"pow", "spirv.GL.Pow"},
            {"sin", "spirv.GL.Sin"},   {"cos", "spirv.GL.Cos"},
            {"abs", "spirv.GL.FAbs"},  {"floor", "spirv.GL.Floor"},
            {"ceil", "spirv.GL.Ceil"}, {"min", "spirv.GL.FMin"},
            {"max", "spirv.GL.FMax"},  {"clamp", "spirv.GL.FClamp"}};
        auto Found = Intrinsics.find(Intrinsic);
        if (Found == Intrinsics.end())
          throw std::runtime_error("unsupported SPIR-V intrinsic: " +
                                   Intrinsic);
        auto Out = NewValue();
        Body << "    " << Out << " = " << Found->second << " ";
        for (unsigned I = 0; I < Op.getNumOperands(); ++I) {
          if (I)
            Body << ", ";
          Body << ValueName(Op.getOperand(I));
        }
        Body << " : " << TypeName(Op.getResult(0).getType()) << "\n";
        Values[Op.getResult(0)] = Out;
      } else if (Name == "kelyra.shader.if") {
        auto Id = NextBlock++;
        auto Merge = "^merge" + std::to_string(Id);
        auto Then = "^then" + std::to_string(Id);
        auto Else = "^else" + std::to_string(Id);
        Body << "    spirv.mlir.selection {\n"
             << "      spirv.BranchConditional " << ValueName(Op.getOperand(0))
             << ", " << Then << ", " << Else << "\n"
             << "    " << Then << ":\n";
        LowerBlock(Op.getRegion(0).front());
        auto *ThenTerm = Op.getRegion(0).front().getTerminator();
        if (ThenTerm->getName().getStringRef() == "kelyra.shader.yield")
          Body << "      spirv.Branch " << Merge << "\n";
        Body << "    " << Else << ":\n";
        LowerBlock(Op.getRegion(1).front());
        auto *ElseTerm = Op.getRegion(1).front().getTerminator();
        if (ElseTerm->getName().getStringRef() == "kelyra.shader.yield")
          Body << "      spirv.Branch " << Merge << "\n";
        Body << "    " << Merge << ":\n"
             << "      spirv.mlir.merge\n"
             << "    }\n";
      } else if (Name == "kelyra.shader.while") {
        auto Id = NextBlock++;
        auto Header = "^header" + std::to_string(Id);
        auto LoopBody = "^body" + std::to_string(Id);
        auto Continue = "^continue" + std::to_string(Id);
        auto Merge = "^loopmerge" + std::to_string(Id);
        Loops.push_back({Continue, Merge});
        Body << "    spirv.mlir.loop {\n"
             << "      spirv.Branch " << Header << "\n"
             << "    " << Header << ":\n";
        auto &ConditionBlock = Op.getRegion(0).front();
        LowerBlock(ConditionBlock);
        auto *Terminator = ConditionBlock.getTerminator();
        if (Terminator->getName().getStringRef() != "kelyra.shader.yield" ||
            Terminator->getNumOperands() != 1)
          throw std::runtime_error("shader while needs a boolean condition");
        Body << "      spirv.BranchConditional "
             << ValueName(Terminator->getOperand(0)) << ", " << LoopBody << ", "
             << Merge << "\n"
             << "    " << LoopBody << ":\n";
        auto &LoopBlock = Op.getRegion(1).front();
        LowerBlock(LoopBlock);
        auto *BodyTerminator = LoopBlock.getTerminator();
        if (BodyTerminator->getName().getStringRef() == "kelyra.shader.yield")
          Body << "      spirv.Branch " << Continue << "\n";
        Body << "    " << Continue << ":\n"
             << "      spirv.Branch " << Header << "\n"
             << "    " << Merge << ":\n"
             << "      spirv.mlir.merge\n"
             << "    }\n";
        Loops.pop_back();
      } else if (Name == "kelyra.shader.break" ||
                 Name == "kelyra.shader.continue") {
        if (Loops.empty())
          throw std::runtime_error("shader loop control outside a loop");
        Body << "    spirv.Branch "
             << (Name == "kelyra.shader.break" ? Loops.back().Merge
                                               : Loops.back().Continue)
             << "\n";
      } else if (Name == "kelyra.shader.return") {
        auto V = Op.getOperand(0);
        Body << "    spirv.ReturnValue " << ValueName(V) << " : "
             << TypeName(V.getType()) << "\n";
      } else if (Name == "kelyra.shader.eval") {
        // The operand-producing op was lowered already.
      } else {
        throw std::runtime_error(
            "unsupported Shader IR op in SPIR-V lowering: " + Name.str());
      }
    }
  }

  void Gather() {
    for (Operation &Op : Input.getBody()->getOperations()) {
      auto Name = Op.getName().getStringRef();
      if (Name == "kelyra.shader.record") {
        RecordInfo Info;
        for (auto Attr : Op.getAttrOfType<ArrayAttr>("fields"))
          Info.Fields.push_back(cast<StringAttr>(Attr).getValue().str());
        for (auto Attr : Op.getAttrOfType<ArrayAttr>("field_types"))
          Info.FieldTypes.push_back(cast<TypeAttr>(Attr).getValue());
        for (auto Attr : Op.getAttrOfType<ArrayAttr>("decorations")) {
          std::vector<std::string> FieldDecorations;
          for (auto Decor : cast<ArrayAttr>(Attr))
            FieldDecorations.push_back(
                cast<StringAttr>(Decor).getValue().str());
          Info.Decorations.push_back(std::move(FieldDecorations));
        }
        Info.Vector = Info.Fields.size() >= 2 && Info.Fields.size() <= 4 &&
                      std::all_of(Info.FieldTypes.begin(),
                                  Info.FieldTypes.end(), [&](Type Ty) {
                                    return Ty == Info.FieldTypes.front() &&
                                           (Ty.isF32() || Ty.isInteger(32));
                                  });
        Records.emplace(AttrString(&Op, "sym_name"), std::move(Info));
      } else if (Name == "kelyra.shader.func") {
        auto FunctionName = AttrString(&Op, "sym_name");
        Functions.emplace(FunctionName, &Op);
        if (AttrString(&Op, "stage") != "helper") {
          if (Entry)
            throw std::runtime_error("Shader IR has multiple entry points");
          Entry = &Op;
        }
      }
    }
    if (!Entry)
      throw std::runtime_error("Shader IR has no entry point");
  }

  void EmitFunctions() {
    for (const auto &[Name, Function] : Functions) {
      NextValue = 0;
      Values.clear();
      Prologue.str({});
      Body.str({});
      auto FunctionTy = cast<FunctionType>(
          Function->getAttrOfType<TypeAttr>("function_type").getValue());
      Source << "  spirv.func @" << Name << "(";
      auto &Block = Function->getRegion(0).front();
      for (unsigned I = 0; I < Block.getNumArguments(); ++I) {
        if (I)
          Source << ", ";
        auto Argument = Block.getArgument(I);
        auto ArgName = "%arg" + std::to_string(I);
        Values[Argument] = ArgName;
        Source << ArgName << " : " << TypeName(Argument.getType());
      }
      Source << ") -> " << TypeName(FunctionTy.getResult(0)) << " \"None\" {\n";
      LowerBlock(Block);
      Source << Prologue.str() << Body.str() << "  }\n";
    }
  }

  static bool HasDecoration(const std::vector<std::string> &Decorations,
                            llvm::StringRef Name) {
    return std::any_of(Decorations.begin(), Decorations.end(),
                       [&](const std::string &D) {
                         return llvm::StringRef(D).starts_with(Name);
                       });
  }

  static unsigned Location(const std::vector<std::string> &Decorations) {
    for (const auto &D : Decorations)
      if (llvm::StringRef(D).starts_with("location:"))
        return static_cast<unsigned>(std::stoul(D.substr(9)));
    throw std::runtime_error("shader interface field needs @location");
  }

  static std::pair<unsigned, unsigned>
  Binding(const std::vector<std::string> &Decorations) {
    for (const auto &D : Decorations) {
      if (!llvm::StringRef(D).starts_with("binding:"))
        continue;
      const auto Separator = D.find(':', 8);
      if (Separator == std::string::npos)
        break;
      return {static_cast<unsigned>(std::stoul(D.substr(8, Separator - 8))),
              static_cast<unsigned>(std::stoul(D.substr(Separator + 1)))};
    }
    throw std::runtime_error("shader resource needs @std.graphics.binding");
  }

  void EmitInterface() {
    auto FunctionTy = cast<FunctionType>(
        Entry->getAttrOfType<TypeAttr>("function_type").getValue());
    auto Stage = AttrString(Entry, "stage");
    auto InputDecorations =
        Entry->getAttrOfType<ArrayAttr>("input_decorations");
    std::vector<std::string> Interface;
    std::set<std::pair<unsigned, unsigned>> UsedBindings;
    for (unsigned I = 0; I < FunctionTy.getNumInputs(); ++I) {
      std::vector<std::string> Decorations;
      for (auto A : cast<ArrayAttr>(InputDecorations[I]))
        Decorations.push_back(cast<StringAttr>(A).getValue().str());
      auto Name = "in" + std::to_string(I);
      auto Ty = TypeName(FunctionTy.getInput(I));
      if (isa<kelyra::ir::ShaderResourceType>(FunctionTy.getInput(I))) {
        const auto [Set, Slot] = Binding(Decorations);
        if (!UsedBindings.insert({Set, Slot}).second)
          throw std::runtime_error("duplicate shader resource set and slot");
        Source << "  spirv.GlobalVariable @" << Name << " bind(" << Set << ", "
               << Slot << ") : !spirv.ptr<" << Ty << ", UniformConstant>\n";
        continue;
      }
      Source << "  spirv.GlobalVariable @" << Name;
      if (HasDecoration(Decorations, "vertex_index"))
        Source << " built_in(\"VertexIndex\")";
      else
        Source << " {location = " << Location(Decorations) << " : i32}";
      Source << " : !spirv.ptr<" << Ty << ", Input>\n";
      Interface.push_back(Name);
    }
    auto OutputTy = FunctionTy.getResult(0);
    const auto &Output = Record(OutputTy);
    for (unsigned I = 0; I < Output.Fields.size(); ++I) {
      auto Name = "out" + std::to_string(I);
      auto Ty = TypeName(Output.FieldTypes[I]);
      Source << "  spirv.GlobalVariable @" << Name;
      if (HasDecoration(Output.Decorations[I], "position"))
        Source << " built_in(\"Position\")";
      else
        Source << " {location = " << Location(Output.Decorations[I])
               << " : i32}";
      Source << " : !spirv.ptr<" << Ty << ", Output>\n";
      Interface.push_back(Name);
    }
    Source << "  spirv.func @main() -> () \"None\" {\n";
    std::vector<std::string> Args;
    for (unsigned I = 0; I < FunctionTy.getNumInputs(); ++I) {
      auto Ty = TypeName(FunctionTy.getInput(I));
      if (isa<kelyra::ir::ShaderResourceType>(FunctionTy.getInput(I))) {
        Source << "    %inptr" << I << " = spirv.mlir.addressof @in" << I
               << " : !spirv.ptr<" << Ty << ", UniformConstant>\n"
               << "    %inval" << I
               << " = spirv.Load \"UniformConstant\" %inptr" << I << " : " << Ty
               << "\n";
        Args.push_back("%inval" + std::to_string(I));
        continue;
      }
      Source << "    %inptr" << I << " = spirv.mlir.addressof @in" << I
             << " : !spirv.ptr<" << Ty << ", Input>\n"
             << "    %inval" << I << " = spirv.Load \"Input\" %inptr" << I
             << " : " << Ty << "\n";
      Args.push_back("%inval" + std::to_string(I));
    }
    Source << "    %result = spirv.FunctionCall @"
           << AttrString(Entry, "sym_name") << "(";
    for (unsigned I = 0; I < Args.size(); ++I) {
      if (I)
        Source << ", ";
      Source << Args[I];
    }
    Source << ") : (";
    for (unsigned I = 0; I < FunctionTy.getNumInputs(); ++I) {
      if (I)
        Source << ", ";
      Source << TypeName(FunctionTy.getInput(I));
    }
    Source << ") -> " << TypeName(OutputTy) << "\n";
    for (unsigned I = 0; I < Output.Fields.size(); ++I) {
      auto Ty = TypeName(Output.FieldTypes[I]);
      Source << "    %field" << I << " = spirv.CompositeExtract %result[" << I
             << " : i32] : " << TypeName(OutputTy) << "\n";
      auto FieldValue = "%field" + std::to_string(I);
      if (Stage == "vertex" &&
          HasDecoration(Output.Decorations[I], "position")) {
        if (Ty != "vector<4xf32>")
          throw std::runtime_error("@position output must be Vec4");
        Source << "    %posy" << I << " = spirv.CompositeExtract " << FieldValue
               << "[1 : i32] : " << Ty << "\n"
               << "    %flipy" << I << " = spirv.FNegate %posy" << I
               << " : f32\n"
               << "    %flipped" << I << " = spirv.CompositeInsert %flipy" << I
               << ", " << FieldValue << "[1 : i32] : f32 into " << Ty << "\n";
        FieldValue = "%flipped" + std::to_string(I);
      }
      Source << "    %outptr" << I << " = spirv.mlir.addressof @out" << I
             << " : !spirv.ptr<" << Ty << ", Output>\n"
             << "    spirv.Store \"Output\" %outptr" << I << ", " << FieldValue
             << " : " << Ty << "\n";
    }
    Source << "    spirv.Return\n  }\n"
           << "  spirv.EntryPoint \""
           << (Stage == "vertex" ? "Vertex" : "Fragment") << "\" @main";
    for (const auto &Name : Interface)
      Source << ", @" << Name;
    Source << "\n";
    if (Stage == "fragment")
      Source << "  spirv.ExecutionMode @main \"OriginUpperLeft\"\n";
  }

public:
  explicit SpirvLowering(ModuleOp Module) : Input(Module) {}

  std::string Run() {
    Gather();
    Source << "spirv.module Logical GLSL450 requires "
              "#spirv.vce<v1.0, [Shader], []> {\n";
    // Interface globals must precede functions. EmitInterface writes the entry
    // wrapper too, so collect it separately and then place helper functions
    // before the wrapper while retaining the globals at module scope.
    EmitInterface();
    auto FullInterface = Source.str();
    auto FunctionStart = FullInterface.find("  spirv.func @main");
    if (FunctionStart == std::string::npos)
      throw std::runtime_error("failed to form SPIR-V entry wrapper");
    auto Globals = FullInterface.substr(0, FunctionStart);
    auto Wrapper = FullInterface.substr(FunctionStart);
    Source.str({});
    Source.clear();
    Source << Globals;
    EmitFunctions();
    Source << Wrapper << "}\n";
    return Source.str();
  }
};

} // namespace

bool kelyra::driver::EmitDirectSpirv(mlir::ModuleOp Module,
                                     llvm::StringRef Output,
                                     std::string &Error) {
  try {
    SpirvLowering Lowering(Module);
    auto Text = Lowering.Run();
    auto *Context = Module.getContext();
    Context->loadDialect<mlir::spirv::SPIRVDialect>();
    auto SpirvModule =
        mlir::parseSourceString<mlir::spirv::ModuleOp>(Text, Context);
    if (!SpirvModule) {
      Error = "failed to parse lowered SPIR-V dialect module:\n" + Text;
      return false;
    }
    if (mlir::failed(mlir::verify(*SpirvModule))) {
      Error = "lowered SPIR-V dialect module failed verification:\n" + Text;
      return false;
    }
    llvm::SmallVector<uint32_t> Binary;
    if (mlir::failed(mlir::spirv::serialize(*SpirvModule, Binary))) {
      Error = "SPIR-V dialect serialization failed:\n" + Text;
      return false;
    }
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC, llvm::sys::fs::OF_None);
    if (EC) {
      Error = "cannot write SPIR-V output: " + EC.message();
      return false;
    }
    Stream.write(reinterpret_cast<const char *>(Binary.data()),
                 Binary.size() * sizeof(uint32_t));
    Stream.flush();
    if (Stream.has_error()) {
      Error = "failed to write SPIR-V binary";
      return false;
    }
    return true;
  } catch (const std::exception &Exception) {
    Error = Exception.what();
    return false;
  }
}
