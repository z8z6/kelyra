#pragma once

#include "Lexer/Lexer.h"
#include "Sema/Sema.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kelyra::codegen {
class IRGen {
  struct Variable {
    sema::Type SemanticType;
    mlir::Value Address;
    mlir::Value DirectValue;
  };

  struct Loop {
    mlir::Block *Break;
    mlir::Block *Continue;
  };

  mlir::MLIRContext &Context;
  const sema::Sema &Analysis;
  unsigned SafeLevel;
  mlir::OpBuilder Builder;
  mlir::Block *FunctionEntry = nullptr;
  std::vector<std::unordered_map<std::string, Variable>> Scopes;
  std::vector<Loop> Loops;
  unsigned GlobalStringCount = 0;

  mlir::Location GetLocation(const lex::Location &Loc);
  mlir::Type GetType(const sema::Type &Type);
  Variable *FindVariable(std::string_view Name);
  mlir::Value CreateAlloca(const sema::Type &Type, mlir::Location Loc);
  mlir::Value EmitAddress(const lex::Node &Expression);
  mlir::Value EmitExpression(const lex::Node &Expression);
  void EmitBlock(const lex::Node &Block);
  void EmitStatement(const lex::Node &Statement);
  void EmitFunction(const lex::Node &Function);

public:
  IRGen(mlir::MLIRContext &Context, const sema::Sema &Analysis,
        unsigned SafeLevel = 0);
  mlir::OwningOpRef<mlir::ModuleOp> Generate(const lex::Node &Module);
  mlir::OwningOpRef<mlir::ModuleOp>
  Generate(llvm::ArrayRef<const lex::Node *> Modules);
};

llvm::Error EmitObject(mlir::ModuleOp Module, llvm::StringRef OutputPath,
                       unsigned OptLevel = 0,
                       llvm::StringRef CWrapperSource = {},
                       llvm::ArrayRef<std::string> CArguments = {});
llvm::Error EmitExecutable(mlir::ModuleOp Module, llvm::StringRef OutputPath,
                           unsigned OptLevel = 0,
                           llvm::ArrayRef<std::string> CSources = {},
                           llvm::ArrayRef<std::string> CArguments = {},
                           llvm::StringRef CWrapperSource = {});
} // namespace kelyra::codegen
