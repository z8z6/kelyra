#include <gtest/gtest.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
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

TEST(CLI, Help) {
  run({"-h"}, 0, "--safe-level", "");
  run({"--help"}, 0, "--safe-level", "");
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

TEST(CLI, RejectInvalidArguments) { run({"--unknown"}, 2, "", "usage:"); }

TEST(CLI, RejectInvalidOptimizationLevel) {
  run({"-O4", "--check", KELYRA_SOURCE_DIR "/examples/basic.kly"}, 2, "",
      "optimization level must be between 0 and 3");
}
