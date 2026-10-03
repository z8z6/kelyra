#pragma once

#include "llvm/Support/CommandLine.h"
#include "llvm/TargetParser/Host.h"
#include <string>

namespace kelyra {
enum class LogLevel { Error, Warn, Info, Debug };

// Options shared by compilation stages. Stage-specific options live with their module.
class Option {
public:
  inline static llvm::cl::OptionCategory KelyraCategory{"Kelyra compiler options"};
  inline static llvm::cl::opt<std::string> InputFile{llvm::cl::Positional,
                                                     llvm::cl::desc("Input Kelyra source file"),
                                                     llvm::cl::Required,
                                                     llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<std::string> OutputFile{
      "o", llvm::cl::desc("Output file"), llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<std::string> Target{
      "target",
      llvm::cl::desc("LLVM target triple (default host)"),
      llvm::cl::init(llvm::sys::getDefaultTargetTriple()),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::list<std::string> ClangArgs{
      "clang-args",
      llvm::cl::desc("Args passed to Clang for C imports and compilation"),
      llvm::cl::ZeroOrMore,
      llvm::cl::cat(Option::KelyraCategory)};
  inline static llvm::cl::opt<bool> DumpMlir{
      "dump-mlir", llvm::cl::desc("Dump MLIR"), llvm::cl::cat(Option::KelyraCategory)};
  inline static llvm::cl::opt<kelyra::LogLevel> LogLevel{
      "log-level",
      llvm::cl::desc("Log level"),
      llvm::cl::values(clEnumValN(kelyra::LogLevel::Error, "error", "Errors only"),
                       clEnumValN(kelyra::LogLevel::Warn, "warn", "Warnings and errors"),
                       clEnumValN(kelyra::LogLevel::Info, "info", "Info, warnings, errors"),
                       clEnumValN(kelyra::LogLevel::Debug, "debug", "Everything")),
      llvm::cl::init(kelyra::LogLevel::Warn),
      llvm::cl::cat(KelyraCategory)};
};
} // namespace kelyra
