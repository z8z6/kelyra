#include "Support/Option.h"
#include "Front/Parser/Parser.h"
#include "Support/Log.h"
#include <iostream>

using namespace kelyra;

namespace {
llvm::cl::opt<bool> DumpAstOption{
    "dump-ast", llvm::cl::desc("Dump the parsed AST"), llvm::cl::cat(Option::KelyraCategory)};
}
void kelyra::lex::ParseResult::DumpAstIfRequested() const {
  if (DumpAstOption && root)
    std::cout << lex::DumpAst(*root) << '\n';
}
bool kelyra::lex::ValidateOutput(bool ExclusiveAction) {
  if (ExclusiveAction && DumpAstOption) {
    kerr() << "--dump-ast cannot be combined with --emit-c-defs\n";
    return false;
  }
  return true;
}
