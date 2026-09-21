#include "CodeGen/IRGen.h"
#include "IR/Kelyra.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/Transforms/Passes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <system_error>
#include <unordered_set>

using namespace kelyra;

namespace {
bool IsTypeNode(lex::TokenKind Kind) {
  using K = lex::TokenKind;
  return Kind == K::ast_type || Kind == K::ast_pointer_type ||
         Kind == K::ast_array_type || Kind == K::ast_result_types ||
         Kind == K::ast_function_type;
}

bool HasTerminator(mlir::Block *Block) {
  return !Block->empty() &&
         Block->back().hasTrait<mlir::OpTrait::IsTerminator>();
}

llvm::OptimizationLevel GetOptimizationLevel(unsigned Level) {
  switch (Level) {
  case 1:
    return llvm::OptimizationLevel::O1;
  case 2:
    return llvm::OptimizationLevel::O2;
  case 3:
    return llvm::OptimizationLevel::O3;
  default:
    return llvm::OptimizationLevel::O0;
  }
}

void Optimize(llvm::Module &Module, llvm::TargetMachine &TargetMachine,
              llvm::OptimizationLevel Level) {
  if (Level == llvm::OptimizationLevel::O0)
    return;
  llvm::LoopAnalysisManager LoopAnalyses;
  llvm::FunctionAnalysisManager FunctionAnalyses;
  llvm::CGSCCAnalysisManager CGSCCAnalyses;
  llvm::ModuleAnalysisManager ModuleAnalyses;
  llvm::PassBuilder Builder(&TargetMachine);
  Builder.registerLoopAnalyses(LoopAnalyses);
  Builder.registerFunctionAnalyses(FunctionAnalyses);
  Builder.registerCGSCCAnalyses(CGSCCAnalyses);
  Builder.registerModuleAnalyses(ModuleAnalyses);
  Builder.crossRegisterProxies(LoopAnalyses, FunctionAnalyses, CGSCCAnalyses,
                               ModuleAnalyses);
  auto Passes = Builder.buildPerModuleDefaultPipeline(Level);
  Passes.run(Module, ModuleAnalyses);
}
} // namespace

codegen::IRGen::IRGen(mlir::MLIRContext &Context, const sema::Sema &Analysis,
                      unsigned SafeLevel, bool DebugInfo)
    : Context(Context), Analysis(Analysis), SafeLevel(SafeLevel),
      DebugInfo(DebugInfo), Builder(&Context) {
  Context.loadDialect<ir::KelyraDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect>();
}

mlir::Location codegen::IRGen::GetLocation(const lex::Location &Loc) {
  mlir::Location Result = mlir::FileLineColLoc::get(
      &Context, Loc.File.empty() ? "<unknown>" : Loc.File, Loc.Line,
      Loc.Column);
  return DebugScope ? mlir::FusedLoc::get(&Context, {Result}, DebugScope)
                    : Result;
}

mlir::Type codegen::IRGen::GetType(const sema::Type &Type) {
  using T = sema::BuiltinType;
  if (Type.IsResults()) {
    llvm::SmallVector<mlir::Type> Results;
    for (const auto &Result : Type.Results)
      Results.push_back(GetType(Result));
    return mlir::LLVM::LLVMStructType::getLiteral(&Context, Results);
  }
  if (Type.IsArray()) {
    return mlir::LLVM::LLVMArrayType::get(GetType(Type.Indexed()),
                                          Type.ArrayLength());
  }
  if (Type.IsPointer() || Type.IsFunction())
    return mlir::LLVM::LLVMPointerType::get(&Context);
  if (Type.IsClass()) {
    llvm::SmallVector<mlir::Type> Fields;
    const auto &Class = *Analysis.GetClass(Type);
    std::uint64_t Offset = 0;
    for (const auto &Field : Class.Fields) {
      if (Offset < Field.Offset)
        Fields.push_back(mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(),
                                                        Field.Offset - Offset));
      Fields.push_back(GetType(Field.Value));
      auto Element = Field.Value;
      std::uint64_t Count = 1;
      while (Element.IsArray()) {
        Count *= Element.ArrayLength();
        Element = Element.Indexed();
      }
      const std::uint64_t Size =
          Count * (Element.IsClass() ? Analysis.GetClass(Element)->Size
                                     : (sema::GetBitWidth(Element) + 7) / 8);
      Offset = Field.Offset + Size;
    }
    if (Offset < Class.Size)
      Fields.push_back(mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(),
                                                      Class.Size - Offset));
    return mlir::LLVM::LLVMStructType::getLiteral(&Context, Fields, true);
  }
  if (Type.IsRecord())
    return mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(),
                                          (sema::GetBitWidth(Type) + 7) / 8);
  mlir::Type Result;
  if (sema::IsInteger(Type.Element))
    Result = Builder.getIntegerType(sema::GetBitWidth(Type));
  else {
    switch (Type.Element) {
    case T::F32:
      Result = Builder.getF32Type();
      break;
    case T::F64:
      Result = Builder.getF64Type();
      break;
    case T::F128:
      Result = Builder.getF128Type();
      break;
    case T::F256:
      Result = ir::F256Type::get(&Context);
      break;
    case T::F512:
      Result = ir::F512Type::get(&Context);
      break;
    case T::Bool:
      Result = Builder.getI1Type();
      break;
    case T::CBool:
      Result = Builder.getIntegerType(sema::GetBitWidth(Type));
      break;
    case T::Char:
      Result = Builder.getI32Type();
      break;
    default:
      llvm_unreachable("unhandled builtin type");
    }
  }
  return Result;
}

codegen::IRGen::Variable *codegen::IRGen::FindVariable(std::string_view Name) {
  for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope) {
    const auto It = Scope->find(std::string(Name));
    if (It != Scope->end())
      return &It->second;
  }
  llvm_unreachable("semantic analysis accepted an unknown variable");
}

mlir::Value codegen::IRGen::CreateAlloca(const sema::Type &Type,
                                         mlir::Location Loc) {
  mlir::OpBuilder::InsertionGuard Guard(Builder);
  Builder.setInsertionPointToStart(FunctionEntry);
  auto One = mlir::arith::ConstantIntOp::create(Builder, Loc, 1, 64);
  return mlir::LLVM::AllocaOp::create(
      Builder, Loc, mlir::LLVM::LLVMPointerType::get(&Context), GetType(Type),
      One,
      Type.IsClass() ? Analysis.GetClass(Type)->Alignment
                     : sema::GetAlignment(Type));
}

mlir::Value codegen::IRGen::EmitAddress(const lex::Node &Expression) {
  using K = lex::TokenKind;
  if (Analysis.GetField(Expression)) {
    mlir::Value Address;
    const sema::ClassInfo *Owner = CurrentClass;
    if (Expression.kind == K::ast_member) {
      const auto &Base = *Expression.children.front();
      const auto &BaseType = Analysis.GetType(Base);
      Owner = Analysis.GetClass(BaseType);
      Address = BaseType.IsPointer() ? EmitExpression(Base) : EmitAddress(Base);
    } else {
      Address = FindVariable("this")->DirectValue;
    }
    return FieldAddress(*Owner, Address, Analysis.GetFieldIndex(Expression),
                        GetLocation(Expression.Loc));
  }
  if (Expression.kind == K::ast_name)
    return FindVariable(Expression.text)->Address;
  if (Expression.kind == K::ast_group)
    return EmitAddress(*Expression.children.front());
  if (Expression.kind == K::ast_unary && Expression.text == "*")
    return EmitExpression(*Expression.children.front());

  const auto &Base = *Expression.children[0];
  const auto BaseType = Analysis.GetType(Base);
  auto BaseAddress = EmitAddress(Base);
  const auto &IndexExpression = *Expression.children[1];
  const auto &IndexType = Analysis.GetType(IndexExpression);
  auto Index = EmitExpression(IndexExpression);
  if (SafeLevel > 0) {
    const auto BitWidth = sema::GetBitWidth(IndexType);
    mlir::Value Valid;
    if (sema::IsSignedInteger(IndexType.Element)) {
      auto Zero = mlir::arith::ConstantIntOp::create(
          Builder, GetLocation(IndexExpression.Loc), GetType(IndexType),
          llvm::APInt(BitWidth, 0));
      Valid = mlir::arith::CmpIOp::create(
          Builder, GetLocation(IndexExpression.Loc),
          mlir::arith::CmpIPredicate::sge, Index, Zero);
    }
    const auto Length = BaseType.Dimensions.front();
    const bool LengthFits =
        BitWidth >= 64 || Length < (std::uint64_t{1} << BitWidth);
    if (LengthFits) {
      auto Limit = mlir::arith::ConstantIntOp::create(
          Builder, GetLocation(IndexExpression.Loc), GetType(IndexType),
          llvm::APInt(BitWidth, Length));
      auto BelowLimit = mlir::arith::CmpIOp::create(
          Builder, GetLocation(IndexExpression.Loc),
          mlir::arith::CmpIPredicate::ult, Index, Limit);
      if (Valid)
        Valid = mlir::arith::AndIOp::create(
            Builder, GetLocation(IndexExpression.Loc), Valid, BelowLimit);
      else
        Valid = BelowLimit;
    }
    if (Valid)
      mlir::cf::AssertOp::create(Builder, GetLocation(IndexExpression.Loc),
                                 Valid, "array index out of bounds");
  }
  auto Zero = mlir::arith::ConstantIntOp::create(
      Builder, GetLocation(Expression.Loc), 0, 32);
  llvm::SmallVector<mlir::Value> Indices{Zero, Index};
  return mlir::LLVM::GEPOp::create(Builder, GetLocation(Expression.Loc),
                                   mlir::LLVM::LLVMPointerType::get(&Context),
                                   GetType(BaseType), BaseAddress, Indices);
}

void codegen::IRGen::EmitFunction(const lex::Node &Function,
                                  const sema::ClassInfo *Owner,
                                  bool DeclarationOnly) {
  using K = lex::TokenKind;
  llvm::SmallVector<const lex::Node *> Parameters;
  const lex::Node *ReturnType = nullptr;
  const lex::Node *Body = nullptr;
  for (const auto &Child : Function.children) {
    if (Child->kind == K::ast_parameter)
      Parameters.push_back(Child.get());
    else if (IsTypeNode(Child->kind) && !Analysis.GetType(*Child).IsVoid())
      ReturnType = Child.get();
    else if (Child->kind == K::ast_block)
      Body = Child.get();
  }

  llvm::SmallVector<mlir::Type> ParameterTypes;
  if (Owner)
    ParameterTypes.push_back(mlir::LLVM::LLVMPointerType::get(&Context));
  for (const auto *Parameter : Parameters)
    ParameterTypes.push_back(GetType(Analysis.GetType(*Parameter)));
  llvm::SmallVector<mlir::Type> Results;
  if (ReturnType)
    Results.push_back(GetType(Analysis.GetType(*ReturnType)));
  auto FunctionType = Builder.getFunctionType(ParameterTypes, Results);
  auto Func =
      mlir::func::FuncOp::create(Builder, GetLocation(Function.Loc),
                                 Analysis.GetSymbol(Function), FunctionType);
  if (DeclarationOnly) {
    // The definition lives in a linked library; emit only the declaration.
    Func.setPrivate();
    return;
  }
  CurrentClass = Owner;
  BeginDebugFunction(Func, Function);
  if (!Analysis.IsPublic(Function) && Function.text != "main")
    Func.setPrivate();
  auto *Entry = Func.addEntryBlock();
  FunctionEntry = Entry;
  Builder.setInsertionPointToStart(Entry);
  Scopes.clear();
  Scopes.emplace_back();
  Cleanups.clear();
  Loops.clear();
  CurrentClass = Owner;
  ActiveDestructor = Function.kind == K::ast_destructor ? Owner : nullptr;
  const unsigned Offset = Owner ? 1 : 0;
  if (Owner) {
    sema::Type Receiver{sema::BuiltinType::Class, {}};
    Receiver.ClassName = Owner->QualifiedName;
    Receiver.AddPointer();
    Scopes.back().emplace("this",
                          Variable{Receiver, {}, Entry->getArgument(0)});
    if (DebugInfo) {
      auto Address = CreateAlloca(Receiver, GetLocation(Function.Loc));
      mlir::LLVM::StoreOp::create(Builder, GetLocation(Function.Loc),
                                  Entry->getArgument(0), Address);
      EmitDebugVariable("this", Function.Loc, Receiver, Address, 1);
    }
  }
  for (std::size_t I = 0; I < Parameters.size(); ++I) {
    const auto Type = Analysis.GetType(*Parameters[I]);
    if (!Type.IsRecord() && sema::GetBitWidth(Type) > 128) {
      Scopes.back().emplace(Parameters[I]->text,
                            Variable{Type, {}, Entry->getArgument(I + Offset)});
      EmitDebugVariable(Parameters[I]->text, Parameters[I]->Loc, Type,
                        Entry->getArgument(I + Offset), I + Offset + 1, true);
      continue;
    }
    auto Address = CreateAlloca(Type, GetLocation(Parameters[I]->Loc));
    mlir::LLVM::StoreOp::create(Builder, GetLocation(Parameters[I]->Loc),
                                Entry->getArgument(I + Offset), Address);
    Scopes.back().emplace(Parameters[I]->text, Variable{Type, Address, {}});
    EmitDebugVariable(Parameters[I]->text, Parameters[I]->Loc, Type, Address,
                      I + Offset + 1);
  }
  EmitBlock(*Body);
  if (!HasTerminator(Builder.getInsertionBlock())) {
    if (ReturnType)
      mlir::LLVM::UnreachableOp::create(Builder, GetLocation(Body->Loc));
    else {
      if (ActiveDestructor)
        EmitFieldDestructors(*ActiveDestructor, Entry->getArgument(0),
                             GetLocation(Body->Loc));
      mlir::func::ReturnOp::create(Builder, GetLocation(Body->Loc));
    }
  }
  CurrentClass = nullptr;
  ActiveDestructor = nullptr;
  DebugScope = {};
}

mlir::OwningOpRef<mlir::ModuleOp>
codegen::IRGen::Generate(const lex::Node &Module) {
  return Generate({&Module});
}

mlir::OwningOpRef<mlir::ModuleOp>
codegen::IRGen::Generate(llvm::ArrayRef<const lex::Node *> Modules) {
  using K = lex::TokenKind;
  if (Modules.empty())
    return {};
  auto Result = mlir::ModuleOp::create(GetLocation(Modules.front()->Loc));
  Builder.setInsertionPointToEnd(Result.getBody());
  for (const auto &External : Analysis.GetExternalFunctions()) {
    if (External.Variadic || External.Return.IsRecord() ||
        std::any_of(External.Parameters.begin(), External.Parameters.end(),
                    [](const sema::Type &Type) { return Type.IsRecord(); }))
      continue;
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : External.Parameters)
      Parameters.push_back(GetType(Parameter));
    llvm::SmallVector<mlir::Type> Results;
    if (!External.Return.IsVoid())
      Results.push_back(GetType(External.Return));
    auto Function = mlir::func::FuncOp::create(
        Builder, Builder.getUnknownLoc(), External.Name,
        Builder.getFunctionType(Parameters, Results));
    Function.setPrivate();
  }
  for (const auto &Wrapper : Analysis.GetCWrappers()) {
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : Wrapper.Parameters)
      Parameters.push_back(GetType(Parameter));
    llvm::SmallVector<mlir::Type> Results;
    if (!Wrapper.ReturnByAddress && !Wrapper.Return.IsVoid())
      Results.push_back(GetType(Wrapper.Return));
    auto Function = mlir::func::FuncOp::create(
        Builder, Builder.getUnknownLoc(), Wrapper.Name,
        Builder.getFunctionType(Parameters, Results));
    Function.setPrivate();
  }
  for (const auto *Module : Modules) {
    const bool External = ExternalModules.count(Module) != 0;
    for (const auto &Child : Module->children) {
      if (Child->kind == K::ast_class) {
        const sema::ClassInfo *Class = nullptr;
        for (const auto &[Name, Candidate] : Analysis.GetClasses())
          if (Candidate.Node == Child.get())
            Class = &Candidate;
        assert(Class);
        for (const auto &Member : Child->children)
          if (Member->kind == K::ast_function ||
              Member->kind == K::ast_constructor ||
              Member->kind == K::ast_destructor) {
            Builder.setInsertionPointToEnd(Result.getBody());
            EmitFunction(*Member, Class, External);
          }
        if (!Class->Constructor) {
          Builder.setInsertionPointToEnd(Result.getBody());
          if (External)
            EmitDefaultConstructorDeclaration(*Class);
          else
            EmitDefaultConstructor(*Class);
        }
        if (!Class->Destructor) {
          Builder.setInsertionPointToEnd(Result.getBody());
          if (External)
            EmitDefaultDestructorDeclaration(*Class);
          else
            EmitDefaultDestructor(*Class);
        }
        continue;
      }
      if (Child->kind != K::ast_function)
        continue;
      Builder.setInsertionPointToEnd(Result.getBody());
      EmitFunction(*Child, nullptr, External);
    }
  }
  return Result;
}

llvm::Error codegen::EmitObject(mlir::ModuleOp Module,
                                llvm::StringRef OutputPath, unsigned OptLevel,
                                llvm::StringRef CWrapperSource,
                                llvm::ArrayRef<std::string> CArguments) {
  mlir::PassManager Passes(Module.getContext());
  Passes.addPass(mlir::createConvertFuncToLLVMPass());
  Passes.addPass(mlir::createArithToLLVMConversionPass());
  Passes.addPass(mlir::createConvertControlFlowToLLVMPass());
  Passes.addPass(mlir::createReconcileUnrealizedCastsPass());
  if (OptLevel == 0) {
    mlir::LLVM::DIScopeForLLVMFuncOpPassOptions DebugOptions;
    DebugOptions.emissionKind = mlir::LLVM::DIEmissionKind::Full;
    Passes.addPass(mlir::LLVM::createDIScopeForLLVMFuncOpPass(DebugOptions));
  }
  if (mlir::failed(Passes.run(Module)))
    return llvm::createStringError("failed to lower MLIR to LLVM dialect");

  mlir::registerBuiltinDialectTranslation(*Module.getContext());
  mlir::registerLLVMDialectTranslation(*Module.getContext());
  llvm::LLVMContext Context;
  std::string BackendError;
  Context.setDiagnosticHandlerCallBack(
      [](const llvm::DiagnosticInfo *Info, void *Opaque) {
        if (Info->getSeverity() != llvm::DS_Error)
          return;
        auto &Message = *static_cast<std::string *>(Opaque);
        llvm::raw_string_ostream Stream(Message);
        llvm::DiagnosticPrinterRawOStream Printer(Stream);
        Info->print(Printer);
      },
      &BackendError);
  auto LLVMModule = mlir::translateModuleToLLVMIR(Module, Context);
  if (!LLVMModule)
    return llvm::createStringError("failed to translate MLIR to LLVM IR");

  if (llvm::InitializeNativeTarget() ||
      llvm::InitializeNativeTargetAsmParser() ||
      llvm::InitializeNativeTargetAsmPrinter())
    return llvm::createStringError("failed to initialize native target");

  const llvm::Triple Triple(llvm::sys::getDefaultTargetTriple());
  std::string TargetError;
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(Triple, TargetError);
  if (!Target)
    return llvm::createStringError(TargetError);

  llvm::TargetOptions Options;
  const auto CodeGenLevel = *llvm::CodeGenOpt::getLevel(OptLevel);
  std::unique_ptr<llvm::TargetMachine> TargetMachine(
      Target->createTargetMachine(Triple, llvm::sys::getHostCPUName(), "",
                                  Options, llvm::Reloc::PIC_, std::nullopt,
                                  CodeGenLevel));
  if (!TargetMachine)
    return llvm::createStringError("failed to create target machine");
  LLVMModule->setDataLayout(TargetMachine->createDataLayout());
  LLVMModule->setTargetTriple(Triple);
  const auto OptimizationLevel = GetOptimizationLevel(OptLevel);
  if (OptimizationLevel != llvm::OptimizationLevel::O0)
    Optimize(*LLVMModule, *TargetMachine, OptimizationLevel);

  llvm::SmallString<128> NativeObject;
  llvm::StringRef NativeOutput = OutputPath;
  if (!CWrapperSource.empty()) {
    if (auto ErrorCode = llvm::sys::fs::createTemporaryFile("kelyra-native",
                                                            "o", NativeObject))
      return llvm::createStringError(ErrorCode,
                                     "cannot create temporary object");
    NativeOutput = NativeObject;
  }
  llvm::FileRemover RemoveNative(NativeObject);
  std::error_code ErrorCode;
  llvm::ToolOutputFile Output(NativeOutput, ErrorCode, llvm::sys::fs::OF_None);
  if (ErrorCode)
    return llvm::createStringError(ErrorCode, "cannot open output file");

  llvm::legacy::PassManager CodeGen;
  if (TargetMachine->addPassesToEmitFile(CodeGen, Output.os(), nullptr,
                                         llvm::CodeGenFileType::ObjectFile))
    return llvm::createStringError("target cannot emit an object file");
  CodeGen.run(*LLVMModule);
  if (!BackendError.empty())
    return llvm::createStringError("%s", BackendError.c_str());
  Output.os().flush();
  if (Output.os().has_error())
    return llvm::createStringError(Output.os().error(),
                                   "cannot write output file");
  Output.keep();
  if (CWrapperSource.empty())
    return llvm::Error::success();

  llvm::SmallString<128> SourcePath, WrapperObject;
  if (auto Code =
          llvm::sys::fs::createTemporaryFile("kelyra-wrapper", "c", SourcePath))
    return llvm::createStringError(Code, "cannot create C wrapper");
  llvm::FileRemover RemoveSource(SourcePath);
  llvm::raw_fd_ostream Source(SourcePath, ErrorCode);
  if (ErrorCode)
    return llvm::createStringError(ErrorCode, "cannot write C wrapper");
  Source << CWrapperSource;
  Source.close();
  if (auto Code = llvm::sys::fs::createTemporaryFile("kelyra-wrapper", "o",
                                                     WrapperObject))
    return llvm::createStringError(Code, "cannot create wrapper object");
  llvm::FileRemover RemoveWrapper(WrapperObject);
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    return llvm::createStringError(Clang.getError(), "cannot find Clang");
  llvm::SmallVector<llvm::StringRef> Compile{*Clang, "-c", SourcePath};
  for (const auto &Argument : CArguments)
    Compile.push_back(Argument);
  Compile.append({"-o", WrapperObject});
  std::string Message;
  if (llvm::sys::ExecuteAndWait(*Clang, Compile, std::nullopt, {}, 0, 0,
                                &Message) != 0)
    return llvm::createStringError("Clang failed to compile C wrapper: %s",
                                   Message.c_str());
  llvm::SmallString<128> Combined;
  llvm::sys::fs::createUniquePath(OutputPath + ".tmp-%%%%%%%%", Combined,
                                  false);
  llvm::FileRemover RemoveCombined(Combined);
  llvm::SmallVector<llvm::StringRef> Link{*Clang,        "-r", NativeObject,
                                          WrapperObject, "-o", Combined};
  if (llvm::sys::ExecuteAndWait(*Clang, Link, std::nullopt, {}, 0, 0,
                                &Message) != 0)
    return llvm::createStringError("Clang failed to combine objects: %s",
                                   Message.c_str());
  if (auto Code = llvm::sys::fs::rename(Combined, OutputPath))
    return llvm::createStringError(Code, "cannot write object file");
  RemoveCombined.releaseFile();
  return llvm::Error::success();
}

llvm::Error codegen::EmitExecutable(mlir::ModuleOp Module,
                                    llvm::StringRef OutputPath,
                                    unsigned OptLevel,
                                    llvm::ArrayRef<std::string> CSources,
                                    llvm::ArrayRef<std::string> CArguments,
                                    llvm::StringRef CWrapperSource) {
  llvm::SmallString<128> ObjectPath;
  if (auto ErrorCode =
          llvm::sys::fs::createTemporaryFile("kelyra", "o", ObjectPath))
    return llvm::createStringError(ErrorCode, "cannot create temporary object");
  llvm::FileRemover RemoveObject(ObjectPath);
  if (auto Error =
          EmitObject(Module, ObjectPath, OptLevel, CWrapperSource, CArguments))
    return Error;

  auto Linker = llvm::sys::findProgramByName(CSources.empty() ? "cc" : "clang");
  if (!Linker)
    return llvm::createStringError(Linker.getError(), "cannot find C compiler");
  llvm::SmallString<128> TemporaryOutput;
  llvm::sys::fs::createUniquePath(OutputPath + ".tmp-%%%%%%%%", TemporaryOutput,
                                  false);
  llvm::FileRemover RemoveOutput(TemporaryOutput);
  llvm::SmallVector<llvm::StringRef> Arguments{*Linker, ObjectPath};
  for (const auto &Source : CSources)
    Arguments.push_back(Source);
  for (const auto &Argument : CArguments)
    Arguments.push_back(Argument);
  Arguments.append({"-o", TemporaryOutput});
  std::string Message;
  const int Status = llvm::sys::ExecuteAndWait(*Linker, Arguments, std::nullopt,
                                               {}, 0, 0, &Message);
  if (Status != 0)
    return llvm::createStringError("linker failed: %s",
                                   Message.empty() ? "non-zero exit status"
                                                   : Message.c_str());
  if (auto ErrorCode = llvm::sys::fs::rename(TemporaryOutput, OutputPath))
    return llvm::createStringError(ErrorCode, "cannot write executable");
  RemoveOutput.releaseFile();
  return llvm::Error::success();
}
