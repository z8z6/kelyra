//
// Created by zzm on 2026/9/18
// Part of RVision
//

#pragma once

#include <llvm/Support/CodeGen.h>
#include <llvm/Support/CommandLine.h>
#include <string>

namespace kelyra {
class Option {
public:
  inline static llvm::cl::OptionCategory KelyraCategory{
      "Kelyra compiler options"};
  inline static llvm::cl::opt<std::string> InputFile{
      llvm::cl::Positional, llvm::cl::desc("Input file"), llvm::cl::Required,
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<std::string> OutputFile{
      "o", llvm::cl::desc("Output file"), llvm::cl::Optional,
    llvm::cl::init("output"),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<llvm::CodeGenOptLevel> OptLevel{
      "O",
      llvm::cl::Prefix,
      llvm::cl::desc("Optimization level (0-3)"),
      llvm::cl::values(
          clEnumVal(llvm::CodeGenOptLevel::None, "No optimization"),
          clEnumVal(llvm::CodeGenOptLevel::Less, "Basic optimization"),
          clEnumVal(llvm::CodeGenOptLevel::Default, "Moderate optimization"),
          clEnumVal(llvm::CodeGenOptLevel::Aggressive,
                    "Aggressive optimization")),
      llvm::cl::init(llvm::CodeGenOptLevel::None),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<unsigned> SafeLevel{
      "safe-level", llvm::cl::desc("Safety level (0 disables checks)"),
      llvm::cl::init(0), llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<bool> Progress{
      "progress",
      llvm::cl::desc("Report build stages and source files on stderr"),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::list<std::string> CSources{
      "c-source", llvm::cl::desc("C source file compiled and linked by Clang"),
      llvm::cl::ZeroOrMore, llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::list<std::string> CArguments{
      "c-arg", llvm::cl::desc("Argument passed to Clang for C imports"),
      llvm::cl::ZeroOrMore, llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::list<std::string> ModulePaths{
      "module-path",
      llvm::cl::desc("Directory searched for imported modules after the entry "
                     "directory; may be repeated"),
      llvm::cl::ZeroOrMore, llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::list<std::string> ExternalPaths{
      "external-path",
      llvm::cl::desc("Directory whose modules are linked instead of compiled: "
                     "only declarations are emitted; may be repeated"),
      llvm::cl::ZeroOrMore, llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::list<std::string> LinkInputs{
      "link-input",
      llvm::cl::desc("Object file or archive linked into the executable; may "
                     "be repeated"),
      llvm::cl::ZeroOrMore, llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<bool> LexDumpAst{
      "dump-ast", llvm::cl::desc("Print the parsed AST"),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<bool> LexVerify{
      "check", llvm::cl::desc("Verify source syntax"),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<bool> EmitMlir{
      "emit-mlir", llvm::cl::desc("Generate and print MLIR"),
    llvm::cl::init(false),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<bool> EmitObject{
      "emit-obj", llvm::cl::desc("Generate a native object file"),
    llvm::cl::init(false),
      llvm::cl::cat(KelyraCategory)};
  inline static llvm::cl::opt<bool> EmitExecutable{
      "emit-exe", llvm::cl::desc("Generate a native executable"),
    llvm::cl::init(true),
      llvm::cl::cat(KelyraCategory)};
};
} // namespace kelyra
