#include "Support/Option.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/Transforms/Passes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"

#include <memory>
#include <optional>
#include <system_error>
#include <vector>

using namespace kelyra;

namespace {
enum class RuntimeMode { Host, Freestanding };
llvm::cl::opt<llvm::CodeGenOptLevel> OptLevel{
    "O",
    llvm::cl::Prefix,
    llvm::cl::desc("Optimization level (0-3)"),
    llvm::cl::values(clEnumValN(llvm::CodeGenOptLevel::None, "0", "No optimization"),
                     clEnumValN(llvm::CodeGenOptLevel::Less, "1", "Basic optimization"),
                     clEnumValN(llvm::CodeGenOptLevel::Default, "2", "Moderate optimization"),
                     clEnumValN(llvm::CodeGenOptLevel::Aggressive, "3", "Aggressive optimization")),
    llvm::cl::init(llvm::CodeGenOptLevel::None),
    llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::opt<RuntimeMode> MainRuntime{
    "main-runtime",
    llvm::cl::desc("Main runtime type"),
    llvm::cl::values(clEnumValN(RuntimeMode::Host, "host", "Host platform"),
                     clEnumValN(RuntimeMode::Freestanding, "freestanding", "Freestanding")),
    llvm::cl::init(RuntimeMode::Host),
    llvm::cl::cat(Option::KelyraCategory)};

llvm::OptimizationLevel GetOptimizationLevel(llvm::CodeGenOptLevel Level) {
  switch (Level) {
  case llvm::CodeGenOptLevel::Less:
    return llvm::OptimizationLevel::O1;
  case llvm::CodeGenOptLevel::Default:
    return llvm::OptimizationLevel::O2;
  case llvm::CodeGenOptLevel::Aggressive:
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
  Builder.crossRegisterProxies(LoopAnalyses, FunctionAnalyses, CGSCCAnalyses, ModuleAnalyses);
  auto Passes = Builder.buildPerModuleDefaultPipeline(Level);
  Passes.run(Module, ModuleAnalyses);
}

llvm::Error AddFreestandingEntry(mlir::ModuleOp Module, const llvm::Triple &Triple) {
  const bool Linux = Triple.getArch() == llvm::Triple::x86_64 && Triple.isOSLinux();
  const bool Windows = Triple.getArch() == llvm::Triple::x86_64 && Triple.isOSWindows();
  if (!Linux && !Windows)
    return llvm::createStringError(
        "freestanding runtime supports only Linux x86-64 and Windows x86-64 "
        "(target: %s)",
        Triple.str().c_str());
  mlir::OpBuilder Builder(Module.getContext());
  Builder.setInsertionPointToEnd(Module.getBody());
  const auto Loc = mlir::FileLineColLoc::get(Module.getContext(), "<kelyra-runtime>", 1, 1);
  const auto I32 = Builder.getI32Type();
  llvm::SmallVector<mlir::Type> Parameters;
  if (Linux)
    Parameters = {Builder.getI64Type(),
                  mlir::LLVM::LLVMPointerType::get(Module.getContext()),
                  mlir::LLVM::LLVMPointerType::get(Module.getContext())};
  auto Start = mlir::func::FuncOp::create(
      Builder, Loc, "__kelyra_start", Builder.getFunctionType(Parameters, {I32}));
  auto *Entry = Start.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  auto Main =
      mlir::func::CallOp::create(Builder, Loc, "main", mlir::TypeRange{I32}, mlir::ValueRange{});
  mlir::func::ReturnOp::create(Builder, Loc, Main.getResults());
  return llvm::Error::success();
}
} // namespace

namespace kelyra::codegen::detail {
bool IsDebugInfoEnabled() { return OptLevel == llvm::CodeGenOptLevel::None; }

bool IsFreestandingRuntime() { return MainRuntime == RuntimeMode::Freestanding; }

llvm::Error EmitObject(mlir::ModuleOp Module, llvm::ArrayRef<std::string> CWrapperSources,
                       llvm::StringRef ObjectPath) {
  const llvm::StringRef OutputPath =
      ObjectPath.empty() ? llvm::StringRef(Option::OutputFile.getValue()) : ObjectPath;
  const llvm::Triple Triple(Option::Target.getValue());
  if (IsFreestandingRuntime())
    if (auto Error = AddFreestandingEntry(Module, Triple))
      return Error;
  std::vector<std::string> AlwaysInlineSymbols;
  std::vector<std::string> GenericSymbols;
  Module.walk([&](mlir::func::FuncOp Function) {
    if (Function->hasAttr("kelyra.generic")) {
      if (!Function.isExternal())
        GenericSymbols.push_back(Function.getSymName().str());
      Function->removeAttr("kelyra.generic");
    }
    if (Function->hasAttr("kelyra.always_inline") && !Function.isExternal()) {
      AlwaysInlineSymbols.push_back(Function.getSymName().str());
      Function->removeAttr("kelyra.always_inline");
    }
  });
  mlir::PassManager Passes(Module.getContext());
  Passes.addPass(mlir::createConvertFuncToLLVMPass());
  Passes.addPass(mlir::createArithToLLVMConversionPass());
  Passes.addPass(mlir::createConvertControlFlowToLLVMPass());
  Passes.addPass(mlir::createReconcileUnrealizedCastsPass());
  if (OptLevel == llvm::CodeGenOptLevel::None) {
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
  for (const auto &Symbol : AlwaysInlineSymbols)
    if (auto *Function = LLVMModule->getFunction(Symbol))
      Function->addFnAttr(llvm::Attribute::AlwaysInline);
  for (const auto &Symbol : GenericSymbols)
    if (auto *Function = LLVMModule->getFunction(Symbol)) {
      Function->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
      auto *Group = LLVMModule->getOrInsertComdat(Symbol);
      Group->setSelectionKind(llvm::Comdat::Any);
      Function->setComdat(Group);
    }
  if (!AlwaysInlineSymbols.empty() && OptLevel == llvm::CodeGenOptLevel::None) {
    llvm::initializeAlwaysInlinerLegacyPassPass(*llvm::PassRegistry::getPassRegistry());
    llvm::legacy::PassManager Inliner;
    Inliner.add(llvm::createAlwaysInlinerLegacyPass());
    Inliner.run(*LLVMModule);
  }

  if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmParser() ||
      llvm::InitializeNativeTargetAsmPrinter())
    return llvm::createStringError("failed to initialize native target");

  std::string TargetError;
  const llvm::Target *Target = llvm::TargetRegistry::lookupTarget(Triple, TargetError);
  if (!Target)
    return llvm::createStringError(TargetError);

  llvm::TargetOptions Options;
  // A library object may contain many modules. Keep unrelated functions and
  // globals discardable so consumers only need the native libraries they use.
  Options.FunctionSections = Triple.isOSWindows();
  Options.DataSections = Triple.isOSWindows();
  const auto CodeGenLevel = OptLevel.getValue();
  std::unique_ptr<llvm::TargetMachine> TargetMachine(
      Target->createTargetMachine(Triple,
                                  llvm::sys::getHostCPUName(),
                                  "",
                                  Options,
                                  llvm::Reloc::PIC_,
                                  std::nullopt,
                                  CodeGenLevel));
  if (!TargetMachine)
    return llvm::createStringError("failed to create target machine");
  LLVMModule->setDataLayout(TargetMachine->createDataLayout());
  LLVMModule->setTargetTriple(Triple);
  if (Triple.isOSWindows())
    for (const auto &CSource : CWrapperSources) {
      auto Clang = llvm::sys::findProgramByName("clang");
      if (!Clang)
        return llvm::createStringError(Clang.getError(), "cannot find Clang");
      llvm::SmallString<128> WrapperSource;
      if (auto Code = llvm::sys::fs::createTemporaryFile("kelyra-wrapper", "c", WrapperSource))
        return llvm::createStringError(Code, "cannot create C wrapper");
      llvm::FileRemover RemoveWrapperSource(WrapperSource);
      std::error_code ErrorCode;
      llvm::raw_fd_ostream Source(WrapperSource, ErrorCode);
      if (ErrorCode)
        return llvm::createStringError(ErrorCode, "cannot write C wrapper");
      Source << CSource;
      Source.close();
      llvm::SmallString<128> BitcodePath;
      if (auto Code = llvm::sys::fs::createTemporaryFile("kelyra-c", "bc", BitcodePath))
        return llvm::createStringError(Code, "cannot create C bitcode");
      llvm::FileRemover RemoveBitcode(BitcodePath);
      const std::string TargetArgument = "--target=" + Triple.str();
      llvm::SmallVector<llvm::StringRef> Compile{
          *Clang, "-emit-llvm", "-c", WrapperSource, TargetArgument};
      for (const auto &Argument : Option::ClangArgs)
        Compile.push_back(Argument);
      Compile.append({"-o", BitcodePath});
      std::string Message;
      if (llvm::sys::ExecuteAndWait(*Clang, Compile, std::nullopt, {}, 0, 0, &Message) != 0)
        return llvm::createStringError("Clang failed to compile C wrapper: %s", Message.c_str());
      llvm::SMDiagnostic Diagnostic;
      auto CModule = llvm::parseIRFile(BitcodePath, Diagnostic, Context);
      if (!CModule)
        return llvm::createStringError("cannot read C wrapper bitcode");
      if (llvm::Linker::linkModules(*LLVMModule, std::move(CModule)))
        return llvm::createStringError("cannot merge C wrapper bitcode");
    }
  const auto OptimizationLevel = GetOptimizationLevel(OptLevel);
  Optimize(*LLVMModule, *TargetMachine, OptimizationLevel);

  llvm::SmallString<128> NativeObject;
  llvm::StringRef NativeOutput = OutputPath;
  if (!Triple.isOSWindows() && !CWrapperSources.empty()) {
    if (auto ErrorCode = llvm::sys::fs::createTemporaryFile("kelyra-native", "o", NativeObject))
      return llvm::createStringError(ErrorCode, "cannot create temporary object");
    NativeOutput = NativeObject;
  }
  llvm::FileRemover RemoveNative(NativeObject);
  std::error_code ErrorCode;
  llvm::ToolOutputFile Output(NativeOutput, ErrorCode, llvm::sys::fs::OF_None);
  if (ErrorCode)
    return llvm::createStringError(ErrorCode, "cannot open output file");

  llvm::legacy::PassManager CodeGen;
  if (TargetMachine->addPassesToEmitFile(
          CodeGen, Output.os(), nullptr, llvm::CodeGenFileType::ObjectFile))
    return llvm::createStringError("target cannot emit an object file");
  CodeGen.run(*LLVMModule);
  if (!BackendError.empty())
    return llvm::createStringError("%s", BackendError.c_str());
  Output.os().flush();
  if (Output.os().has_error())
    return llvm::createStringError(Output.os().error(), "cannot write output file");
  Output.keep();
  if (Triple.isOSWindows() || CWrapperSources.empty())
    return llvm::Error::success();

  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    return llvm::createStringError(Clang.getError(), "cannot find Clang");
  std::vector<std::string> Objects;
  std::vector<std::unique_ptr<llvm::FileRemover>> RemoveObjects;
  std::string Message;
  for (const auto &CSource : CWrapperSources) {
    llvm::SmallString<128> WrapperSource;
    if (auto Code = llvm::sys::fs::createTemporaryFile("kelyra-wrapper", "c", WrapperSource))
      return llvm::createStringError(Code, "cannot create C wrapper");
    llvm::FileRemover RemoveWrapperSource(WrapperSource);
    llvm::raw_fd_ostream Source(WrapperSource, ErrorCode);
    if (ErrorCode)
      return llvm::createStringError(ErrorCode, "cannot write C wrapper");
    Source << CSource;
    Source.close();
    llvm::SmallString<128> CObject;
    if (auto Code = llvm::sys::fs::createTemporaryFile("kelyra-c", "o", CObject))
      return llvm::createStringError(Code, "cannot create C object");
    Objects.push_back(CObject.str().str());
    RemoveObjects.push_back(std::make_unique<llvm::FileRemover>(CObject));
    const std::string TargetArgument = "--target=" + Triple.str();
    llvm::SmallVector<llvm::StringRef> Compile{*Clang, "-c", WrapperSource, TargetArgument};
    for (const auto &Argument : Option::ClangArgs)
      Compile.push_back(Argument);
    Compile.append({"-o", CObject});
    if (llvm::sys::ExecuteAndWait(*Clang, Compile, std::nullopt, {}, 0, 0, &Message) != 0)
      return llvm::createStringError("Clang failed to compile C wrapper: %s", Message.c_str());
  }
  const std::string TargetArgument = "--target=" + Triple.str();
  llvm::SmallString<128> Combined;
  llvm::sys::fs::createUniquePath(OutputPath + ".tmp-%%%%%%%%", Combined, false);
  llvm::FileRemover RemoveCombined(Combined);
  llvm::SmallVector<llvm::StringRef> Link{*Clang, "-r", NativeObject, TargetArgument};
  for (const auto &Object : Objects)
    Link.push_back(Object);
  Link.append({"-o", Combined});
  if (llvm::sys::ExecuteAndWait(*Clang, Link, std::nullopt, {}, 0, 0, &Message) != 0)
    return llvm::createStringError("Clang failed to combine objects: %s", Message.c_str());
  if (auto Code = llvm::sys::fs::rename(Combined, OutputPath))
    return llvm::createStringError(Code, "cannot write object file");
  RemoveCombined.releaseFile();
  return llvm::Error::success();
}

} // namespace kelyra::codegen::detail
