#include <gtest/gtest.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <optional>
#include <string>

namespace {
void run(llvm::ArrayRef<llvm::StringRef> arguments, int expectedCode,
         llvm::StringRef expectedOut, llvm::StringRef expectedErr) {
  llvm::SmallString<128> outPath, errPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-cli", "out", outPath));
  llvm::FileRemover removeOut(outPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-cli", "err", errPath));
  llvm::FileRemover removeErr(errPath);
  llvm::SmallVector<llvm::StringRef> args{KELYRA_EXECUTABLE};
  args.append(arguments.begin(), arguments.end());
  std::string error;
  const int code = llvm::sys::ExecuteAndWait(
      KELYRA_EXECUTABLE, args, std::nullopt,
      {llvm::StringRef(""), outPath.str(), errPath.str()}, 10, 0, &error);
  ASSERT_EQ(code, expectedCode) << error;
  auto output = llvm::MemoryBuffer::getFile(outPath);
  auto errors = llvm::MemoryBuffer::getFile(errPath);
  ASSERT_TRUE(bool(output));
  ASSERT_TRUE(bool(errors));
  const auto out = (*output)->getBuffer();
  const auto err = (*errors)->getBuffer();
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

TEST(CLI, CheckValidSource) {
  run({"--check", KELYRA_SOURCE_DIR "/examples/basic.kly"}, 0, "", "");
}

TEST(CLI, ImportsDoNotExposeTransitiveModules) {
  constexpr llvm::StringLiteral ModulePath("--module-path=" KELYRA_TEST_DIR
                                           "/modules");
  run({"--check", ModulePath, KELYRA_TEST_DIR "/modules/transitive/direct.kly"},
      0, "", "");
  run({"--check", ModulePath,
       KELYRA_TEST_DIR "/modules/transitive/indirect_function.kly"},
      1, "", "declaration is private to another module");
  run({"--check", ModulePath,
       KELYRA_TEST_DIR "/modules/transitive/indirect_type.kly"},
      1, "", "declaration is private to another module");
}

TEST(CLI, NamedEntrypoint) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-entry-named", "exe",
                                                  ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/entry_named.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
  run({"--emit-exe", KELYRA_TEST_DIR "/add.kly"}, 1, "",
      "executable requires one @main function");
}

TEST(CLI, InlineAndDeprecatedAnnotations) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-inline-deprecated",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath,
       KELYRA_TEST_DIR "/inline_deprecated.kly"},
      0, "",
      "warning: use of deprecated function 'old_increment': use "
      "increment");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, ClassReturn) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-class-return", "exe",
                                                  ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/class_return.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, GenericClassAndFunction) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic", "exe",
                                                  ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/generic.kly"}, 0,
      "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, GenericMultipleAndNestedTypes) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic-nested",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath,
       KELYRA_TEST_DIR "/generic_nested.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, RejectGenericArgumentCountMismatch) {
  run({"--emit-obj", KELYRA_TEST_DIR "/generic_invalid_arity.kly"}, 1, "",
      "wrong number of generic type arguments");
}

TEST(CLI, GenericFromLinkedModule) {
  llvm::SmallString<128> ObjectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic-library", "o",
                                                  ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-generic-linked",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-obj", "-o", ObjectPath,
       KELYRA_TEST_DIR "/generic_modules/library.kly"},
      0, "", "");
  const auto External = "--external-path=" KELYRA_TEST_DIR "/generic_modules";
  const auto LinkInput = std::string("--link-input=") + ObjectPath.str().str();
  run({"--emit-exe", External, LinkInput, "-o", ExecutablePath,
       KELYRA_TEST_DIR "/generic_modules/main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, QualifiedBuiltinAnnotations) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-builtin-qualified",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath,
       KELYRA_TEST_DIR "/builtin_qualified.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, EntrypointInImportedModule) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-imported-entry",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath,
       KELYRA_TEST_DIR "/modules/entry_import.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

#if defined(__linux__)
TEST(CLI, TargetConditionalCompilation) {
  run({"--check", KELYRA_TEST_DIR "/cfg/main.kly"}, 0, "", "");
  run({"--check", "--module-path=" KELYRA_TEST_DIR,
       KELYRA_TEST_DIR "/cfg/module_gate.kly"},
      0, "", "");
  run({"--check", "--target=x86_64-pc-windows-msvc",
       KELYRA_TEST_DIR "/cfg/module_linux.kly"},
      1, "", "module is disabled by @cfg for target");
}
#endif

TEST(CLI, TargetConditionalCompilationForWindows) {
  run({"--check", "--target=x86_64-pc-windows-msvc",
       KELYRA_TEST_DIR "/cfg/windows.kly"},
      0, "", "");
}

TEST(CLI, RejectInvalidTargetCondition) {
  run({"--check", KELYRA_TEST_DIR "/cfg/invalid.kly"}, 1, "",
      "invalid @cfg annotation");
  run({"--check", KELYRA_TEST_DIR "/cfg/invalid_old_key.kly"}, 1, "",
      "invalid @cfg annotation");
  run({"--check", KELYRA_TEST_DIR "/cfg/invalid_duplicate.kly"}, 1, "",
      "invalid @cfg annotation");
}

TEST(CLI, WarnModulePathMismatch) {
  run({"--check", KELYRA_TEST_DIR "/path_mismatch.kly"}, 0, "",
      "module 'incorrect.name' does not match path suffix");
}

TEST(CLI, Help) {
  run({"-h"}, 0, "--safe-level", "");
  run({"--help"}, 0, "--safe-level", "");
}

TEST(CLI, ReportCompilationProgress) {
  run({"--progress", "--emit-mlir", KELYRA_TEST_DIR "/modules/main.kly"}, 0,
      "func.func", "[codegen]");
  run({"--progress", "--check", KELYRA_TEST_DIR "/modules/main.kly"}, 0, "",
      "vector.kly");
}

TEST(CLI, LogLevelFiltersProgress) {
  run({"--progress", "--log-level=error", "--check",
       KELYRA_TEST_DIR "/modules/main.kly"},
      0, "", "");
}

TEST(CLI, ClassConstructionAndRAII) {
  for (const auto Optimization : {"-O0", "-O3"}) {
    llvm::SmallString<128> executablePath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-raii", "exe",
                                                    executablePath));
    llvm::FileRemover removeExecutable(executablePath);
    run({Optimization, "--emit-exe", "--c-source=" KELYRA_TEST_DIR "/c/trace.c",
         "-o", executablePath, KELYRA_TEST_DIR "/class_raii.kly"},
        0, "", "");
    llvm::SmallVector<llvm::StringRef> args{executablePath};
    std::string error;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                        10, 0, &error),
              0)
        << error;
  }
}

TEST(CLI, ClassCopyMoveAndValueParameter) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-class-transfer",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath,
       KELYRA_TEST_DIR "/class_transfer.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, InterfaceConstantAndAbstractMethod) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-interface", "exe",
                                                  ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath, KELYRA_TEST_DIR "/interface.kly"}, 0,
      "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            42)
      << Error;
}

TEST(CLI, CustomClassCopyMove) {
  llvm::SmallString<128> ExecutablePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-custom-transfer",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "-o", ExecutablePath,
       KELYRA_TEST_DIR "/class_custom_transfer.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Args, std::nullopt, {},
                                      10, 0, &Error),
            27)
      << Error;
}

TEST(CLI, DumpAst) {
  run({"--dump-ast", KELYRA_SOURCE_DIR "/examples/basic.kly"}, 0,
      "Function \"sum\"", "");
}

TEST(CLI, RejectInvalidSource) {
  run({"--check", KELYRA_TEST_DIR "/invalid.kly"}, 1, "", "error:");
}

TEST(CLI, EmitMlir) {
  run({"--emit-mlir", KELYRA_TEST_DIR "/add.kly"}, 0, "arith.addi", "");
}

TEST(CLI, EmitObject) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-cli", "o", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"-O0", "--emit-obj", "-o", objectPath, KELYRA_TEST_DIR "/add.kly"}, 0,
      "", "");
  auto object = llvm::MemoryBuffer::getFile(objectPath);
  ASSERT_TRUE(bool(object));
  EXPECT_FALSE((*object)->getBuffer().empty());
  EXPECT_TRUE((*object)->getBuffer().contains("add.kly"));
}

TEST(CLI, EmitOptimizedObject) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-cli", "o", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"-O3", "--emit-obj", "-o", objectPath, KELYRA_TEST_DIR "/add.kly"}, 0,
      "", "");
}

TEST(CLI, EmitExecutable) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-cli-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/main.kly"}, 0, "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, ExplicitNumericCast) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-cast-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/cast.kly"}, 0, "",
      "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

#if defined(__linux__)
TEST(CLI, ExternalFunctionDeclaration) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-extern-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/extern.kly"}, 0,
      "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}
#endif

TEST(CLI, RejectExternalFunctionBody) {
  run({"--emit-mlir", KELYRA_TEST_DIR "/invalid_extern.kly"}, 1, "",
      "invalid @extern function declaration");
}

TEST(CLI, RejectUnsupportedCallingConvention) {
  run({"--emit-mlir", KELYRA_TEST_DIR "/invalid_callconv.kly"}, 1, "",
      "invalid @extern function declaration");
}

#if defined(__linux__) && defined(__x86_64__)
TEST(CLI, EmitFreestandingExecutable) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-freestanding-%%%%%%%%",
                                  executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "--runtime=freestanding", "-o", executablePath,
       KELYRA_TEST_DIR "/main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;

  if (auto readelf = llvm::sys::findProgramByName("readelf")) {
    llvm::SmallString<128> outputPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "kelyra-freestanding-readelf", "out", outputPath));
    llvm::FileRemover removeOutput(outputPath);
    llvm::SmallVector<llvm::StringRef> inspect{*readelf, "-l", executablePath};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(
                  *readelf, inspect, std::nullopt,
                  {llvm::StringRef(""), outputPath, llvm::StringRef("")}, 10),
              0);
    auto output = llvm::MemoryBuffer::getFile(outputPath);
    ASSERT_TRUE(bool(output));
    EXPECT_FALSE((*output)->getBuffer().contains("INTERP"));
  }
}

TEST(CLI, EmitWindowsFreestandingObject) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-win-start", "obj",
                                                  objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"--emit-obj", "--runtime=freestanding",
       "--target=x86_64-pc-windows-msvc", "-o", objectPath,
       KELYRA_TEST_DIR "/main.kly"},
      0, "", "");
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
  run({"--runtime=unknown", "--check", KELYRA_TEST_DIR "/main.kly"}, 2, "",
      "--runtime must be 'host' or 'freestanding'");
}

TEST(CLI, InlineAssemblyAndForwardFunction) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-asm-%%%%%%%%", executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/asm_forward.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, RejectInvalidInlineAssemblyRegister) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-invalid-asm", "o",
                                                  objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"--emit-obj", "-o", objectPath, KELYRA_TEST_DIR "/invalid_asm.kly"}, 1,
      "", "could not allocate output register");
}

TEST(CLI, PointerAddressAndDereference) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-pointer-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/pointer.kly"}, 0,
      "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, EmitExecutableWithModules) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-modules-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/modules/main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, EmitExecutableWithWildcardImport) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-wildcard-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath,
       KELYRA_TEST_DIR "/modules/wildcard_main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, EmitExecutableWithModulePath) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-module-path-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "--module-path=" KELYRA_TEST_DIR "/module_path", "-o",
       executablePath, KELYRA_TEST_DIR "/module_path_main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, RejectModuleOutsideSearchPath) {
  run({"--check", KELYRA_TEST_DIR "/module_path_main.kly"}, 1, "",
      "cannot find module 'external.util'");
}

TEST(CLI, LinkExternalModule) {
  llvm::SmallString<128> objectPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-external", "o", objectPath));
  llvm::FileRemover removeObject(objectPath);
  run({"--emit-obj", "--module-path=" KELYRA_TEST_DIR "/external/library", "-o",
       objectPath, KELYRA_TEST_DIR "/external/library/external/math.kly"},
      0, "", "");
  llvm::SmallString<256> linkArgument("--link-input=");
  linkArgument += objectPath;
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-external-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  // The external path covers the entry too; the entry is still compiled.
  run({"--emit-exe", "--module-path=" KELYRA_TEST_DIR "/external/library",
       "--external-path=" KELYRA_TEST_DIR "/external", linkArgument, "-o",
       executablePath, KELYRA_TEST_DIR "/external/main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            42)
      << error;
}

TEST(CLI, ReflectFieldsFromLinkedModule) {
  llvm::SmallString<128> ObjectPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("kelyra-reflect-external",
                                                  "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  run({"--emit-obj", "-o", ObjectPath,
       KELYRA_TEST_DIR "/reflection/lib/reflected/library.kly"},
      0, "", "");
  llvm::SmallString<256> LinkArgument("--link-input=");
  LinkArgument += ObjectPath;
  llvm::SmallString<128> ExecutablePath;
  llvm::sys::fs::createUniquePath("kelyra-reflect-external-%%%%%%%%",
                                  ExecutablePath, true);
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  run({"--emit-exe", "--runtime=freestanding",
       "--module-path=" KELYRA_TEST_DIR "/reflection/lib",
       "--module-path=" KELYRA_SOURCE_DIR "/../kstd/src",
       "--external-path=" KELYRA_TEST_DIR "/reflection/lib", LinkArgument, "-o",
       ExecutablePath, KELYRA_TEST_DIR "/reflection/main.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Arguments{ExecutablePath};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, Arguments, std::nullopt,
                                      {}, 10, 0, &Error),
            0)
      << Error;
}

TEST(CLI, RejectExternalModuleWithoutLinkInput) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-external-missing-%%%%%%%%",
                                  executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "--module-path=" KELYRA_TEST_DIR "/external/library",
       "--external-path=" KELYRA_TEST_DIR "/external/library", "-o",
       executablePath, KELYRA_TEST_DIR "/external/main.kly"},
      1, "", "linker failed");
}

TEST(CLI, CallCFunction) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-c-call-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "--c-source=" KELYRA_TEST_DIR "/c/print.c", "-o",
       executablePath, KELYRA_TEST_DIR "/c/main.kly"},
      0, "", "");

  llvm::SmallString<128> outputPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-c-call", "out", outputPath));
  llvm::FileRemover removeOutput(outputPath);
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                executablePath, args, std::nullopt,
                {llvm::StringRef(""), outputPath.str(), llvm::StringRef("")},
                10, 0, &error),
            0)
      << error;
  auto output = llvm::MemoryBuffer::getFile(outputPath);
  ASSERT_TRUE(bool(output));
  EXPECT_TRUE((*output)->getBuffer().contains("hello from C"));
}

TEST(CLI, GenerateImportableCDefinitions) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("kelyra-c-defs", Directory));
  llvm::SmallString<128> Definitions(Directory);
  llvm::sys::path::append(Definitions, "generated.kly");
  llvm::FileRemover RemoveDefinitions(Definitions);
  run({"--emit-c-defs", "--c-defs-module=generated", "-o", Definitions,
       KELYRA_TEST_DIR "/c/main.kly"},
      0, "", "");
  auto Generated = llvm::MemoryBuffer::getFile(Definitions);
  ASSERT_TRUE(bool(Generated));
  EXPECT_TRUE((*Generated)->getBuffer().contains("module generated;"));
  EXPECT_TRUE((*Generated)->getBuffer().contains("pub fn make_pair"));
  llvm::SmallString<128> Executable;
  llvm::sys::fs::createUniquePath("kelyra-c-defs-app-%%%%%%%%", Executable,
                                  true);
  llvm::FileRemover RemoveExecutable(Executable);
  llvm::SmallString<256> ModulePath("--module-path=");
  ModulePath += Directory;
  run({"--emit-exe", ModulePath, "--c-source=" KELYRA_TEST_DIR "/c/print.c",
       "-o", Executable, KELYRA_TEST_DIR "/c/generated_user.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> Args{Executable};
  std::string Error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, Args, std::nullopt, {}, 10, 0,
                                      &Error),
            0)
      << Error;
}

TEST(CLI, ObjectIncludesCSource) {
  llvm::SmallString<128> Object;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-c-bundle", "o", Object));
  llvm::FileRemover RemoveObject(Object);
  run({"--emit-obj", "--c-source=" KELYRA_TEST_DIR "/c/print.c", "-o", Object,
       KELYRA_TEST_DIR "/c/main.kly"},
      0, "", "");
  llvm::SmallString<128> Executable;
  llvm::sys::fs::createUniquePath("kelyra-c-bundle-%%%%%%%%", Executable, true);
  llvm::FileRemover RemoveExecutable(Executable);
  auto Clang = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Clang));
  llvm::SmallVector<llvm::StringRef> Link{*Clang, Object, "-o", Executable};
  std::string Error;
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(*Clang, Link, std::nullopt, {}, 10, 0, &Error),
      0)
      << Error;
  llvm::SmallVector<llvm::StringRef> Args{Executable};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, Args, std::nullopt, {}, 10, 0,
                                      &Error),
            0)
      << Error;
}

TEST(CLI, CallCWithStructPointerAndVarargs) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-c-ffi-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "--c-source=" KELYRA_TEST_DIR "/c/print.c", "-o",
       executablePath, KELYRA_TEST_DIR "/c/ffi.kly"},
      0, "", "");

  llvm::SmallString<128> outputPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("kelyra-c-ffi", "out", outputPath));
  llvm::FileRemover removeOutput(outputPath);
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                executablePath, args, std::nullopt,
                {llvm::StringRef(""), outputPath.str(), llvm::StringRef("")},
                10, 0, &error),
            0)
      << error;
  auto output = llvm::MemoryBuffer::getFile(outputPath);
  ASSERT_TRUE(bool(output));
  EXPECT_TRUE((*output)->getBuffer().contains("total=126"));
}

TEST(CLI, RejectPrivateModuleFunction) {
  run({"--emit-mlir", KELYRA_TEST_DIR "/modules/private_main.kly"}, 1, "",
      "declaration is private to another module");
}

TEST(CLI, GeneratedDefaultConstructor) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-default-init-%%%%%%%%",
                                  executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath, KELYRA_TEST_DIR "/default_init.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            7)
      << error;
}

TEST(CLI, BridgePointerAndSizeToCTypes) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-memory-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "--c-source=" KELYRA_TEST_DIR "/c/memory.c", "-o",
       executablePath, KELYRA_TEST_DIR "/c/memory.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            0)
      << error;
}

TEST(CLI, RunNQueens) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-n-queens-%%%%%%%%", executablePath,
                                  true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"--emit-exe", "-o", executablePath,
       KELYRA_SOURCE_DIR "/examples/n_queens.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            92)
      << error;
}

TEST(CLI, RejectOutOfBoundsIndex) {
  llvm::SmallString<128> executablePath;
  llvm::sys::fs::createUniquePath("kelyra-out-of-bounds-%%%%%%%%",
                                  executablePath, true);
  llvm::FileRemover removeExecutable(executablePath);
  run({"-O3", "--safe-level=2", "--emit-exe", "-o", executablePath,
       KELYRA_TEST_DIR "/out_of_bounds.kly"},
      0, "", "");
  llvm::SmallVector<llvm::StringRef> args{executablePath};
  std::string error;
  EXPECT_NE(llvm::sys::ExecuteAndWait(executablePath, args, std::nullopt, {},
                                      10, 0, &error),
            0);
}

TEST(CLI, RejectInvalidArguments) {
  run({"--unknown"}, 2, "USAGE:", "Unknown command line argument");
}

TEST(CLI, RejectInvalidOptimizationLevel) {
  run({"-O4", "--check", KELYRA_SOURCE_DIR "/examples/basic.kly"}, 2,
      "USAGE:", "Cannot find option named '4'");
}
