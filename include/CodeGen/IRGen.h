#pragma once

#include "Lexer/Lexer.h"
#include "Sema/Sema.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <set>
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
    std::size_t CleanupDepth;
  };
  struct Cleanup {
    const sema::ClassInfo *Class;
    mlir::Value Address;
  };

  mlir::MLIRContext &Context;
  const sema::Sema &Analysis;
  unsigned SafeLevel;
  bool DebugInfo;
  mlir::LLVM::DIScopeAttr DebugScope;
  std::unordered_map<std::string, mlir::LLVM::DITypeAttr> DebugClasses;
  mlir::OpBuilder Builder;
  mlir::Block *FunctionEntry = nullptr;
  std::vector<std::unordered_map<std::string, Variable>> Scopes;
  std::vector<Loop> Loops;
  std::vector<std::vector<Cleanup>> Cleanups;
  const sema::ClassInfo *CurrentClass = nullptr;
  const sema::ClassInfo *ActiveDestructor = nullptr;
  unsigned GlobalStringCount = 0;
  std::set<const lex::Node *> ExternalModules;

  mlir::Location GetLocation(const lex::Location &Loc);
  mlir::LLVM::DIFileAttr GetDebugFile(const lex::Location &Loc);
  mlir::LLVM::DITypeAttr GetDebugType(const sema::Type &Type);
  void BeginDebugFunction(mlir::Operation *Function, const lex::Node &Node);
  void EmitDebugVariable(std::string_view Name, const lex::Location &Loc,
                         const sema::Type &Type, mlir::Value Storage,
                         unsigned Argument = 0, bool DirectValue = false);
  mlir::Type GetType(const sema::Type &Type);
  Variable *FindVariable(std::string_view Name);
  mlir::Value CreateAlloca(const sema::Type &Type, mlir::Location Loc);
  mlir::Value EmitAddress(const lex::Node &Expression);
  mlir::Value EmitNameExpression(const lex::Node &Expression);
  mlir::Value EmitIndexExpression(const lex::Node &Expression);
  mlir::Value EmitCallExpression(const lex::Node &Expression);
  mlir::Value EmitLiteralExpression(const lex::Node &Expression);
  mlir::Value EmitGroupExpression(const lex::Node &Expression);
  mlir::Value EmitUnaryExpression(const lex::Node &Expression);
  mlir::Value EmitBinaryExpression(const lex::Node &Expression);
  mlir::Value EmitExpression(const lex::Node &Expression);
  void EmitBlock(const lex::Node &Block);
  void EmitBlockStatement(const lex::Node &Statement);
  void EmitLetStatement(const lex::Node &Statement);
  void EmitAssignStatement(const lex::Node &Statement);
  void EmitExpressionStatement(const lex::Node &Statement);
  void EmitAsmStatement(const lex::Node &Statement);
  void EmitReturnStatement(const lex::Node &Statement);
  void EmitBreakStatement(const lex::Node &Statement);
  void EmitContinueStatement(const lex::Node &Statement);
  void EmitIfStatement(const lex::Node &Statement);
  void EmitWhenStatement(const lex::Node &Statement);
  void EmitWhileStatement(const lex::Node &Statement);
  void EmitStatement(const lex::Node &Statement);
  void EmitFunction(const lex::Node &Function,
                    const sema::ClassInfo *Owner = nullptr,
                    bool DeclarationOnly = false);
  void EmitConstruction(const lex::Node &Expression, mlir::Value Address);
  void EmitCleanups(std::size_t KeepDepth, mlir::Location Loc);
  void EmitFieldDestructors(const sema::ClassInfo &Class, mlir::Value Address,
                            mlir::Location Loc);
  void EmitDefaultDestructor(const sema::ClassInfo &Class);
  void EmitDefaultDestructorDeclaration(const sema::ClassInfo &Class);
  mlir::Value FieldAddress(const sema::ClassInfo &Class, mlir::Value Address,
                           std::size_t Index, mlir::Location Loc);

public:
  IRGen(mlir::MLIRContext &Context, const sema::Sema &Analysis,
        unsigned SafeLevel = 0, bool DebugInfo = true);
  // Modules in this set are provided by a linked library, so only their
  // declarations are emitted and their definitions are left to the linker.
  void SetExternalModules(const std::set<const lex::Node *> &Modules) {
    ExternalModules = Modules;
  }
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
