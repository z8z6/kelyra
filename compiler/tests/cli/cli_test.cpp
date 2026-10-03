#include <gtest/gtest.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include "llvm/ADT/ScopeExit.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {
void run(llvm::ArrayRef<llvm::StringRef> arguments, int expectedCode, llvm::StringRef expectedOut,
         llvm::StringRef expectedErr) {
  llvm::SmallString<128> outPath, errPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-cli", "out", outPath));
  llvm::FileRemover removeOut(outPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-cli", "err", errPath));
  llvm::FileRemover removeErr(errPath);
  llvm::SmallVector<llvm::StringRef> args{KELYRA_EXECUTABLE};
  args.append(arguments.begin(), arguments.end());
  std::string error;
  const int code = llvm::sys::ExecuteAndWait(KELYRA_EXECUTABLE,
                                             args,
                                             std::nullopt,
                                             {llvm::StringRef(""), outPath.str(), errPath.str()},
                                             10,
                                             0,
                                             &error);
  auto output = llvm::MemoryBuffer::getFile(outPath);
  auto errors = llvm::MemoryBuffer::getFile(errPath);
  ASSERT_TRUE(bool(output));
  ASSERT_TRUE(bool(errors));
  const auto out = (*output)->getBuffer();
  const auto err = (*errors)->getBuffer();
  ASSERT_EQ(code, expectedCode) << error << "\nstdout:\n"
                                << out.str() << "\nstderr:\n"
                                << err.str();
  if (expectedOut.empty())
    EXPECT_TRUE(out.empty()) << out.str();
  else
    EXPECT_TRUE(out.contains(expectedOut)) << out.str();
  if (expectedErr.empty())
    EXPECT_TRUE(err.empty()) << err.str();
  else
    EXPECT_TRUE(err.contains(expectedErr)) << err.str();
}
} // namespace

TEST(CLI, AnalyzeValidSource) { run({KELYRA_SOURCE_DIR "/examples/basic.kly"}, 0, "", ""); }

TEST(CLI, BooleanOperatorsShortCircuit) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-short-circuit", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/short_circuit.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, ImportStandardLibraryWithoutModulePath) {
  run({KELYRA_TEST_DIR "/stdlib_imports.kly"}, 0, "", "");
}

TEST(CLI, MetaDeclarationsAreOmittedFromRuntime) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-meta-declarations-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/meta_declarations.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 7)
      << Error;
}

TEST(CLI, MetaBlockCompilesToRuntimeConstant) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-meta-block-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/meta_block.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 7)
      << Error;
}

TEST(CLI, MetaAnnotationIncludesInjectedRuntimeDependencies) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-meta-injected-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_TEST_DIR "/meta_annotation_modules",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/meta_annotation_modules/main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, ConditionalAnnotationMembers) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-annotation-when-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/annotation_when.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, RejectNonBooleanAnnotationCondition) {
  run({KELYRA_TEST_DIR "/invalid_annotation_when.kly"},
      1,
      "",
      "annotation when requires a compile-time boolean");
}

TEST(CLI, ImportsDoNotExposeTransitiveModules) {
  constexpr llvm::StringLiteral ModulePath("--module-search-path=" KELYRA_TEST_DIR "/modules");
  run({ModulePath, KELYRA_TEST_DIR "/modules/transitive/direct.kly"}, 0, "", "");
  run({ModulePath, KELYRA_TEST_DIR "/modules/transitive/indirect_function.kly"},
      1,
      "",
      "declaration is private to another module");
  run({ModulePath, KELYRA_TEST_DIR "/modules/transitive/indirect_type.kly"},
      1,
      "",
      "declaration is private to another module");
}

TEST(CLI, NamedEntrypoint) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-entry-named", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/entry_named.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/add.kly"},
      1,
      "",
      "executable requires one @main function");
}

TEST(CLI, InlineAndDeprecatedAnnotations) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-inline-deprecated", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/inline_deprecated.kly"},
      0,
      "",
      "warning: use of deprecated function 'old_increment': use "
      "increment");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, ClassReturn) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-class-return", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/class_return.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, GenericClassAndFunction) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/generic.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, SliceValuesAndRanges) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-slice", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/slice.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, RejectReadOnlySliceWrite) {
  run({KELYRA_TEST_DIR "/invalid_readonly_slice.kly"}, 1, "", "invalid assignment target");
  run({KELYRA_TEST_DIR "/invalid_readonly_slice_address.kly"}, 1, "", "invalid assignment target");
}

TEST(CLI, RejectInvalidSliceRangeAtRuntime) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-slice-bounds", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/invalid_slice_range.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_NE(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, GenericMultipleAndNestedTypes) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic-nested", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/generic_nested.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, RejectGenericArgumentCountMismatch) {
  run({KELYRA_TEST_DIR "/generic_invalid_arity.kly"},
      1,
      "",
      "wrong number of generic type arguments");
}

TEST(CLI, RejectGenericFunctionTemplateOverload) {
  run({KELYRA_TEST_DIR "/generic_overload_reject.kly"}, 1, "", "duplicate generic declaration");
}

TEST(CLI, QualifiedBuiltinAnnotations) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-builtin-qualified", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/builtin_qualified.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, EntrypointInImportedModule) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-imported-entry", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/modules/entry_import.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

#if defined(__linux__)
TEST(CLI, TargetConditionalCompilation) {
  run({KELYRA_TEST_DIR "/cfg/main.kly"}, 0, "", "");
  run({"--module-search-path=" KELYRA_TEST_DIR, KELYRA_TEST_DIR "/cfg/module_gate.kly"}, 0, "", "");
  run({"--target=x86_64-pc-windows-msvc", KELYRA_TEST_DIR "/cfg/module_linux.kly"},
      1,
      "",
      "module is disabled by @cfg for target");
}
#endif

TEST(CLI, TargetConditionalCompilationForWindows) {
  run({"--target=x86_64-pc-windows-msvc", KELYRA_TEST_DIR "/cfg/windows.kly"}, 0, "", "");
}

TEST(CLI, RejectInvalidTargetCondition) {
  run({KELYRA_TEST_DIR "/cfg/invalid.kly"}, 1, "", "invalid @cfg annotation");
  run({KELYRA_TEST_DIR "/cfg/invalid_old_key.kly"}, 1, "", "invalid @cfg annotation");
  run({KELYRA_TEST_DIR "/cfg/invalid_duplicate.kly"}, 1, "", "invalid @cfg annotation");
  run({KELYRA_TEST_DIR "/cfg/invalid_string.kly"}, 1, "", "invalid @cfg annotation");
}

TEST(CLI, WarnModulePathMismatch) {
  run({KELYRA_TEST_DIR "/path_mismatch.kly"},
      0,
      "",
      "module 'incorrect.name' does not match path suffix");
}

TEST(CLI, Help) {
  run({"-h"}, 0, "--safe-level", "");
  run({"--help"}, 0, "--safe-level", "");
}

TEST(CLI, ClassConstructionAndRAII) {
  for (const auto Optimization : {"-O0", "-O3"}) {
    llvm::SmallString<128> executablePath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-raii", "exe", executablePath));
    llvm::FileRemover removeExecutable(executablePath);
    run({Optimization,
         "--emit-exe",
         "--link=" KELYRA_TEST_DIR "/c/trace.c",
         "-o",
         executablePath,
         KELYRA_TEST_DIR "/class_raii.kly"},
        0,
        "",
        "");
    llvm::SmallVector<llvm::StringRef> args{executablePath};
    std::string error;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 0)
        << error;
  }
}

TEST(CLI, ClassCopyMoveAndValueParameter) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-class-transfer", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/class_transfer.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, InterfaceConstantAndAbstractMethod) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-interface", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/interface.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, CustomClassCopyMove) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-custom-transfer", "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/class_custom_transfer.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 27)
      << Error;
}

TEST(CLI, DumpAst) {
  run({"--dump-ast", KELYRA_SOURCE_DIR "/examples/basic.kly"}, 0, "Function \"sum\"", "");
}

TEST(CLI, RejectInvalidSource) { run({KELYRA_TEST_DIR "/invalid.kly"}, 1, "", "error:"); }

TEST(CLI, EmitMlir) { run({"--dump-mlir", KELYRA_TEST_DIR "/add.kly"}, 0, "arith.addi", ""); }

TEST(CLI, EmitObject) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-cli", "o", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"-O0", "--emit-obj", "-o", objectPath, KELYRA_TEST_DIR "/add.kly"}, 0, "", "");
  auto object = llvm::MemoryBuffer::getFile(objectPath);
  ASSERT_TRUE(bool(object));
  EXPECT_FALSE((*object)->getBuffer().empty());
  EXPECT_TRUE((*object)->getBuffer().contains("add.kly"));
}

TEST(CLI, EmitOptimizedObject) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-cli", "o", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"-O3", "--emit-obj", "-o", objectPath, KELYRA_TEST_DIR "/add.kly"}, 0, "", "");
}

TEST(CLI, EmitExecutable) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-cli-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/main.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, ExplicitNumericCast) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-cast-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/cast.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

#if defined(__linux__)
TEST(CLI, ExternalFunctionDeclaration) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-extern-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/extern.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, ExternalFunctionReturnsCallback) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-extern-callback-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--link=" KELYRA_TEST_DIR "/c/return_callback.c",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/extern_callback.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}
#endif

TEST(CLI, SingleInheritanceVirtualDispatch) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-inheritance-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/inheritance.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, FinalClassCannotBeInherited) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/invalid_final_inheritance.kly"}, 1, "", "invalid class");
}

TEST(CLI, DeepInheritanceVirtualDispatch) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-inheritance-deep-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/inheritance_deep.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 3)
      << Error;
}

TEST(CLI, RejectInvalidInheritanceOverride) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/invalid_inheritance_override.kly"}, 1, "", "invalid class");
}

TEST(CLI, DerivedConstructorRequiresSuper) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/invalid_inheritance_super.kly"},
      1,
      "",
      "initialize every field once");
}

TEST(CLI, InterfacePolymorphism) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-interface-poly-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/interface_polymorphism.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, ForInStructuralIterator) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-for-in-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_SOURCE_DIR "/../kstd/src",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/for_in.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, RejectInvalidForIterator) {
  run({KELYRA_TEST_DIR "/invalid_for_in.kly"},
      1,
      "",
      "for-in requires iter()/next() and a valid iterator");
}

TEST(CLI, MaybeInlineRawStorage) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-maybe-raw-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_SOURCE_DIR "/../kstd/src",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/maybe_raw.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, InterfaceInheritancePolymorphism) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-interface-inherit-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/interface_inheritance.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, InterfacePolymorphismAcrossModules) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-interface-module-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_TEST_DIR "/modules",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/interface_module_main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, DumpClassLayout) {
  run({"--dump-class-layout", KELYRA_TEST_DIR "/inheritance.kly"},
      0,
      "+0 size=16 align=8 index=0 $base: Base",
      "");
  run({"--dump-class-layout", KELYRA_TEST_DIR "/inheritance.kly"},
      0,
      "+8 size=8 align=8 index=2 $virtual.read#0_: fn",
      "");
}

TEST(CLI, GenericAliases) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-types-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/generic_aliases.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, ScopedAliases) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-scoped-aliases-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/scoped_aliases.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, GenericAliasAcrossModules) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-alias-module-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/alias_modules/main.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, RejectRecursiveTypeAlias) {
  run({KELYRA_TEST_DIR "/invalid_type_cycle.kly"}, 1, "", "unknown builtin type");
}

TEST(CLI, RejectGenericAliasArityMismatch) {
  run({KELYRA_TEST_DIR "/invalid_alias_arity.kly"},
      1,
      "",
      "wrong number of generic type arguments");
}

TEST(CLI, AnnotationCompositionAndMembers) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-annotation-members-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/annotation_members.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, RejectAnnotationCompositionCycle) {
  run({KELYRA_TEST_DIR "/invalid_annotation_cycle.kly"}, 1, "", "cyclic annotation composition");
}

TEST(CLI, RejectAnnotationMemberConflict) {
  run({KELYRA_TEST_DIR "/invalid_annotation_member_conflict.kly"},
      1,
      "",
      "invalid class or duplicate member");
}

TEST(CLI, ComposedFinalPreventsInheritance) {
  run({KELYRA_TEST_DIR "/invalid_composed_final.kly"}, 1, "", "invalid class");
}

TEST(CLI, ImportedAnnotationComposition) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-imported-annotation-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/annotation_modules/main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, GenericConstructorForwarding) {
  for (const auto *Source : {"generic_constructor_forward.kly",
                             "generic_constructor_forward_class.kly",
                             "forward_modules/main.kly"}) {
    llvm::SmallString<128> ExecutablePath;
    llvm::sys::fs::createUniquePath("kelyra-forward-%%%%%%%%", ExecutablePath, true);
    llvm::FileRemover RemoveExecutable(ExecutablePath);
    const auto Input = std::string(KELYRA_TEST_DIR) + "/" + Source;
    run({"--emit-exe", "-o", ExecutablePath, Input}, 0, "", "");
    llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
    std::string Error;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
        << Source << ": " << Error;
  }
}

TEST(CLI, GenericFunctionParameterPack) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-function-pack-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/generic_function_pack.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, StaticClassMembers) {
  for (const auto *Source : {"static_members.kly", "static_modules/main.kly"}) {
    llvm::SmallString<128> ExecutablePath;
    llvm::sys::fs::createUniquePath("kelyra-static-%%%%%%%%", ExecutablePath, true);
    llvm::FileRemover RemoveExecutable(ExecutablePath);
    const auto Input = std::string(KELYRA_TEST_DIR) + "/" + Source;
    run({"--emit-exe", "-o", ExecutablePath, Input}, 0, "", "");
    llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
    std::string Error;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
        << Source << ": " << Error;
  }
}

TEST(CLI, RejectInvalidStaticMembers) {
  run({KELYRA_TEST_DIR "/invalid_static_target.kly"}, 1, "", "invalid class");
  run({KELYRA_TEST_DIR "/invalid_static_receiver.kly"}, 1, "", "unknown name");
  run({KELYRA_TEST_DIR "/invalid_static_instance_call.kly"}, 1, "", "ordinary methods");
  run({KELYRA_TEST_DIR "/invalid_static_value.kly"}, 1, "", "unknown builtin type");
}

TEST(CLI, RejectInvalidForwardConstructors) {
  run({KELYRA_TEST_DIR "/invalid_forward_missing_annotation.kly"}, 1, "", "invalid class");
  run({KELYRA_TEST_DIR "/invalid_forward_parameter_target.kly"},
      1,
      "",
      "annotation is not valid on this declaration");
}

TEST(CLI, CTypesComeFromStandardLibrary) {
  run({KELYRA_TEST_DIR "/extern.kly"}, 0, "", "");
  run({KELYRA_TEST_DIR "/invalid_c_intrinsic.kly"}, 1, "", "unknown builtin type");
}

TEST(CLI, RejectExternalFunctionBody) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/invalid_extern.kly"},
      1,
      "",
      "invalid @extern function declaration");
}

TEST(CLI, RejectUnsupportedCallingConvention) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/invalid_callconv.kly"},
      1,
      "",
      "invalid annotation declaration or argument");
}

#if defined(__linux__) && defined(__x86_64__)
TEST(CLI, EmitFreestandingExecutable) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-freestanding-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe",
       "--main-runtime=freestanding",
       "-o",
       executablePath,
       KELYRA_TEST_DIR "/main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;

  if (auto readelf = llvm::sys::findProgramByName("readelf")) {
    llvm::SmallString<128> outputPath;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("kelyra-freestanding-readelf", "out", outputPath));
    llvm::FileRemover removeOutput(outputPath);
    llvm::SmallVector<llvm::StringRef> inspect{*readelf, "-l", executablePath};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(*readelf,
                                        inspect,
                                        std::nullopt,
                                        {llvm::StringRef(""), outputPath, llvm::StringRef("")},
                                        10),
              0);
    auto output = llvm::MemoryBuffer::getFile(outputPath);
    ASSERT_TRUE(bool(output));
    EXPECT_FALSE((*output)->getBuffer().contains("INTERP"));
  }
}

TEST(CLI, EmitWindowsFreestandingObject) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-win-start", "obj", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"--emit-obj",
       "--main-runtime=freestanding",
       "--target=x86_64-pc-windows-msvc",
       "-o",
       objectPath,
       KELYRA_TEST_DIR "/main.kly"},
      0,
      "",
      "");
  auto object = llvm::MemoryBuffer::getFile(objectPath);
  ASSERT_TRUE(bool(object));
  const auto bytes = (*object)->getBuffer();
  ASSERT_GE(bytes.size(), 2u);
  EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0x64);
  EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 0x86);
  EXPECT_TRUE(bytes.contains("__kelyra_start"));
}
#endif

TEST(CLI, RejectInvalidRuntime) {
  run({"--main-runtime=unknown", KELYRA_TEST_DIR "/main.kly"},
      2,
      "USAGE:",
      "Cannot find option named 'unknown'");
}

TEST(CLI, InlineAssemblyAndForwardFunction) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-asm-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/asm_forward.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, RejectInvalidInlineAssemblyRegister) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-invalid-asm", "o", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"--emit-obj", "-o", objectPath, KELYRA_TEST_DIR "/invalid_asm.kly"},
      1,
      "",
      "could not allocate output register");
}

TEST(CLI, PointerAddressAndDereference) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-pointer-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/pointer.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, EmitExecutableWithModules) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-modules-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/modules/main.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, EmitExecutableWithUnqualifiedImport) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-wildcard-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/modules/wildcard_main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, EmitExecutableWithModulePath) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-module-path-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_TEST_DIR "/module_path",
       "-o",
       executablePath,
       KELYRA_TEST_DIR "/module_path_main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 42)
      << error;
}

TEST(CLI, EmitExecutableWithDirectoryModule) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-directory-module-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_TEST_DIR "/module_path",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/module_path_directory_main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, Accessors) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-accessors-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/accessors.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, IdentifierInterpolation) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-interpolation-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/identifier_interpolation.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, RejectInvalidIdentifierInterpolation) {
  run({KELYRA_TEST_DIR "/invalid_identifier_interpolation.kly"},
      1,
      "",
      "identifier interpolation requires a compile-time string");
}

TEST(CLI, RejectAccessorNameConflict) {
  run({KELYRA_TEST_DIR "/invalid_accessors_conflict.kly"}, 1, "", "duplicate function");
}

TEST(CLI, Aspect) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-aspect-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--module-search-path=" KELYRA_SOURCE_DIR "/../kstd/src",
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/aspect.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, RejectModuleOutsideSearchPath) {
  run({KELYRA_TEST_DIR "/module_path_main.kly"}, 1, "", "cannot find module 'external.util'");
}

TEST(CLI, LibraryInterfaceIncludesUnimportedModules) {
  llvm::SmallString<128> ObjectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-library", "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  const std::string InterfacePath = ObjectPath.str().str() + ".kmi";
  llvm::FileRemover RemoveInterface(InterfacePath);
  const std::string OutputPath = ObjectPath.str().str() + ".exe";
  llvm::FileRemover RemoveOutput(OutputPath);
  const std::string Root = KELYRA_TEST_DIR "/external/library";
  run({"--emit-obj",
       "--library-root=" + Root,
       "--emit-interface=" + InterfacePath,
       "-o",
       ObjectPath,
       KELYRA_TEST_DIR "/external/library/external/math.kly"},
      0,
      "",
      "");
  run({"--emit-exe",
       "--module-interface-path=" + InterfacePath,
       "--link=" + ObjectPath.str().str(),
       "-o",
       OutputPath,
       KELYRA_TEST_DIR "/external/interface_main.kly"},
      0,
      "",
      "");
  run({"--target=aarch64-unknown-linux-gnu",
       "--module-interface-path=" + InterfacePath,
       KELYRA_TEST_DIR "/external/interface_main.kly"},
      1,
      "",
      "cannot load module interface");
  llvm::SmallVector<llvm::StringRef> Args{OutputPath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(OutputPath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, GenericFromLibraryInterface) {
  llvm::SmallString<128> ArchivePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic-archive", "o", ArchivePath));
  llvm::FileRemover RemoveArchive(ArchivePath);
  const std::string InterfacePath = ArchivePath.str().str() + ".kmi";
  llvm::FileRemover RemoveInterface(InterfacePath);
  const std::string OutputPath = ArchivePath.str().str() + ".exe";
  llvm::FileRemover RemoveOutput(OutputPath);
  run({"--emit-obj",
       "--library-root=" KELYRA_TEST_DIR "/generic_modules",
       "--emit-interface=" + InterfacePath,
       "-o",
       ArchivePath,
       KELYRA_TEST_DIR "/generic_modules/library.kly"},
      0,
      "",
      "");
  run({"--emit-exe",
       "--module-interface-path=" + InterfacePath,
       "--link=" + ArchivePath.str().str(),
       "-o",
       OutputPath,
       KELYRA_TEST_DIR "/generic_interface_main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{OutputPath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(OutputPath, Args, std::nullopt, {}, 10, 0, &Error), 42)
      << Error;
}

TEST(CLI, ReflectFieldsFromLinkedModule) {
  llvm::SmallString<128> ObjectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-reflect-external", "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  const std::string InterfacePath = ObjectPath.str().str() + ".kmi";
  llvm::FileRemover RemoveInterface(InterfacePath);
  run({"--emit-obj",
       "--library-root=" KELYRA_TEST_DIR "/reflection/lib",
       "--emit-interface=" + InterfacePath,
       "-o",
       ObjectPath,
       KELYRA_TEST_DIR "/reflection/lib/reflected/library.kly"},
      0,
      "",
      "");
  llvm::SmallString<256> LinkArgument("--link=");
  LinkArgument += ObjectPath;
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-reflect-external-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe",
       "--main-runtime=freestanding",
       "--module-interface-path=" + InterfacePath,
       LinkArgument,
       "-o",
       ExecutablePath,
       KELYRA_TEST_DIR "/reflection/main.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Arguments{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Arguments, std::nullopt, {}, 10, 0, &Error),
            0)
      << Error;
}

TEST(CLI, CallCFunction) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-c-call-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe",
       "--link=" KELYRA_TEST_DIR "/c/print.c",
       "-o",
       executablePath,
       KELYRA_TEST_DIR "/c/main.kly"},
      0,
      "",
      "");

  llvm::SmallString<128> outputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-c-call", "out", outputPath));
  llvm::FileRemover removeOutput(outputPath);
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(executablePath,
                                      args,
                                      std::nullopt,
                                      {llvm::StringRef(""), outputPath.str(), llvm::StringRef("")},
                                      10,
                                      0,
                                      &error),
            0)
      << error;
  auto output = llvm::MemoryBuffer::getFile(outputPath);
  ASSERT_TRUE(bool(output));
  EXPECT_TRUE((*output)->getBuffer().contains("hello from C"));
}

TEST(CLI, GenerateImportableCDefinitions) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("kelyra-c-defs", Directory));
  llvm::SmallString<128> Definitions(Directory);
  llvm::sys::path::append(Definitions, "generated.kly");
  llvm::FileRemover RemoveDefinitions(Definitions);
  run({"--emit-c-defs",
       "--c-defs-module=generated",
       "-o",
       Definitions,
       KELYRA_TEST_DIR "/c/main.kly"},
      0,
      "",
      "");
  auto Generated = llvm::MemoryBuffer::getFile(Definitions);
  ASSERT_TRUE(bool(Generated));
  EXPECT_TRUE((*Generated)->getBuffer().contains("module generated;"));
  EXPECT_TRUE((*Generated)->getBuffer().contains("pub fn make_pair"));
  llvm::SmallString<128> Executable;
  llvm::sys::fs::createUniquePath("kelyra-c-defs-app-%%%%%%%%", Executable, true);
  llvm::FileRemover RemoveExecutable(Executable);
  llvm::SmallString<256> ModulePath("--module-search-path=");
  ModulePath += Directory;
  run({"--emit-exe",
       ModulePath,
       "--link=" KELYRA_TEST_DIR "/c/print.c",
       "-o",
       Executable,
       KELYRA_TEST_DIR "/c/generated_user.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> Args{Executable};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, Args, std::nullopt, {}, 10, 0, &Error), 0)
      << Error;
}

TEST(CLI, CallCWithStructPointerAndVarargs) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-c-ffi-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe",
       "--link=" KELYRA_TEST_DIR "/c/print.c",
       "-o",
       executablePath,
       KELYRA_TEST_DIR "/c/ffi.kly"},
      0,
      "",
      "");

  llvm::SmallString<128> outputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-c-ffi", "out", outputPath));
  llvm::FileRemover removeOutput(outputPath);
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(executablePath,
                                      args,
                                      std::nullopt,
                                      {llvm::StringRef(""), outputPath.str(), llvm::StringRef("")},
                                      10,
                                      0,
                                      &error),
            0)
      << error;
  auto output = llvm::MemoryBuffer::getFile(outputPath);
  ASSERT_TRUE(bool(output));
  EXPECT_TRUE((*output)->getBuffer().contains("total=126"));
}

TEST(CLI, RejectImportedCBitfieldAccess) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/c/invalid_bitfield.kly"},
      1,
      "",
      "C bit field, flexible array, or unaligned field cannot be accessed "
      "directly");
}

TEST(CLI, RejectInvalidCHeaderWithoutCrashing) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/c/invalid_header.kly"},
      1,
      "",
      "Clang could not parse C header");
}

TEST(CLI, RejectPrivateModuleFunction) {
  run({"--dump-mlir", KELYRA_TEST_DIR "/modules/private_main.kly"},
      1,
      "",
      "declaration is private to another module");
}

TEST(CLI, GeneratedDefaultConstructor) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-default-init-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/default_init.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 7)
      << error;
}

TEST(CLI, EnumMatchAndBlockExpressions) {
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-enum-match-%%%%%%%%", ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/enum_match.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {}, 10, 0, &Error), 16)
      << Error;
}

TEST(CLI, BridgePointerAndSizeToCTypes) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-memory-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe",
       "--link=" KELYRA_TEST_DIR "/c/memory.c",
       "-o",
       executablePath,
       KELYRA_TEST_DIR "/c/memory.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 0)
      << error;
}

TEST(CLI, RunNQueens) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-n-queens-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_SOURCE_DIR "/examples/n_queens.kly"}, 0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 92)
      << error;
}

TEST(CLI, RejectOutOfBoundsIndex) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-out-of-bounds-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"-O3",
       "--safe-level=1",
       "--emit-exe",
       "-o",
       executablePath,
       KELYRA_TEST_DIR "/out_of_bounds.kly"},
      0,
      "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_NE(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {}, 10, 0, &error), 0);
}

TEST(CLI, RejectInvalidArguments) {
  run({"--unknown"}, 2, "USAGE:", "Unknown command line argument");
}

TEST(CLI, RejectInvalidOptimizationLevel) {
  run({"-O4", KELYRA_SOURCE_DIR "/examples/basic.kly"},
      2,
      "USAGE:",
      "Cannot find option named '4'");
}

TEST(CLI, RelocatedInterfaceRetainsCDeclarationsAndGenericBridges) {
  namespace fs = std::filesystem;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("kelyra-c-interface", Directory));
  const fs::path Root(Directory.c_str());
  auto Cleanup = llvm::scope_exit([&] {
    std::error_code Error;
    fs::remove_all(Root, Error);
  });
  const auto Source = Root / "source";
  const auto Package = Root / "package";
  fs::create_directory(Source);
  fs::create_directory(Package);
  const auto Write = [](const fs::path &Path, const char *Text) {
    std::ofstream Stream(Path);
    Stream << Text;
    return bool(Stream);
  };
  ASSERT_TRUE(Write(Source / "shared.h", "typedef struct Pair { int left, right; } Pair;\n"));
  ASSERT_TRUE(Write(Source / "pair.h",
                    "#include \"shared.h\"\nPair make_pair(void);\nint sum_pair(Pair);\n"));
  ASSERT_TRUE(Write(Source / "bridge.kly", R"(
module bridge;
import c "pair.h";
pub fn pair() -> c.Pair { return c.make_pair(); }
pub fn sum<T>(value: T) -> i32 { return c.sum_pair(value); }
)"));
  ASSERT_TRUE(Write(Root / "pair.c", R"(
typedef struct Pair { int left, right; } Pair;
Pair make_pair(void) { Pair value = {20, 22}; return value; }
int sum_pair(Pair value) { return value.left + value.right; }
)"));
  ASSERT_TRUE(Write(Root / "main.kly", R"(
import bridge;
import c;
@main
pub fn main() -> i32 { return bridge.sum<c.Pair>(bridge.pair()); }
)"));
  const auto Archive = (Root / "bridge.lib").string();
  const auto Interface = Archive + ".kmi";
  const auto LibraryRoot = "--library-root=" + Source.string();
  const auto InterfaceOutput = "--emit-interface=" + Interface;
  const auto Input = (Source / "bridge.kly").string();
  ASSERT_NO_FATAL_FAILURE(
      run({"--emit-obj", LibraryRoot, InterfaceOutput, "-o", Archive, Input}, 0, "", ""));
  fs::rename(Source, Root / "hidden-source");
  fs::rename(Archive, Package / "bridge.lib");
  fs::rename(Interface, Package / "bridge.kmi");
  const auto InterfaceInput = "--module-interface-path=" + (Package / "bridge.kmi").string();
  const auto LinkLibrary = "--link=" + (Package / "bridge.lib").string();
  const auto LinkC = "--link=" + (Root / "pair.c").string();
  const auto Main = (Root / "main.kly").string();
  const auto Executable = (Root / "main.exe").string();
  ASSERT_NO_FATAL_FAILURE(
      run({"--emit-exe", InterfaceInput, LinkLibrary, LinkC, "-o", Executable, Main}, 0, "", ""));
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt, {}, 10, 0, &Error),
            42)
      << Error;
}
