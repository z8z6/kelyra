#pragma once

#include "Lexer/Lexer.h"
#include "Sema/Type.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kelyra::sema {
struct ModuleInput {
  const lex::Node *Ast;
  bool IsEntry = false;
};

struct ExternalFunction {
  std::string Name;
  std::string Header;
  std::vector<Type> Parameters;
  Type Return{BuiltinType::CInt, {}};
  unsigned CallingConvention = 0;
  bool Variadic = false;
};

struct ExternalType {
  std::string Name;
  Type Value;
};

struct CWrapper {
  std::string Name;
  std::string Source;
  std::vector<Type> Parameters;
  Type Return{BuiltinType::CInt, {}};
  std::vector<bool> ParametersByAddress;
  bool ReturnByAddress = false;
};

class Sema {
  struct FunctionInfo {
    const lex::Node *Node = nullptr;
    std::string Module;
    std::string Symbol;
    std::vector<Type> Parameters;
    Type Return{BuiltinType::I32, {}};
    bool Public = false;
    bool Variadic = false;
    const ExternalFunction *External = nullptr;
  };

  std::vector<lex::Diagnostic> Diagnostics;
  std::unordered_map<const lex::Node *, Type> Types;
  std::unordered_map<std::string, FunctionInfo> Functions;
  std::unordered_map<const lex::Node *, std::string> Symbols;
  std::unordered_map<const lex::Node *, std::string> Callees;
  std::unordered_map<const lex::Node *, std::size_t> CWrapperCalls;
  std::unordered_map<std::string, std::unordered_set<std::string>> Imports;
  std::unordered_map<std::string, Type> ExternalTypes;
  std::vector<ExternalFunction> ExternalFunctions;
  std::vector<CWrapper> CWrappers;
  std::vector<std::unordered_map<std::string, Type>> Scopes;
  std::optional<Type> ReturnType;
  std::string CurrentModule;

  void Error(const lex::Node &Node, lex::DiagnosticKind Kind);
  std::optional<Type> CheckType(const lex::Node &Node);
  void CheckFunction(const lex::Node &Function);
  void CheckBlock(const lex::Node &Block, unsigned LoopDepth = 0);
  void CheckStatement(const lex::Node &Statement, unsigned LoopDepth);
  std::optional<Type>
  CheckExpression(const lex::Node &Expression,
                  std::optional<Type> Expected = std::nullopt,
                  bool Negated = false);
  const Type *FindName(std::string_view Name) const;
  bool AlwaysReturns(const lex::Node &Node) const;

public:
  bool Check(const lex::Node &Module);
  bool
  CheckModules(const std::vector<ModuleInput> &Modules,
               const std::vector<ExternalFunction> &ExternalDeclarations = {},
               const std::vector<ExternalType> &ExternalTypeDeclarations = {});
  bool CheckEntrypoint(const lex::Node &Module);
  const std::vector<lex::Diagnostic> &GetDiagnostics() const {
    return Diagnostics;
  }
  const Type &GetType(const lex::Node &Node) const { return Types.at(&Node); }
  const std::string &GetSymbol(const lex::Node &Node) const {
    return Symbols.at(&Node);
  }
  const std::string &GetCallee(const lex::Node &Node) const {
    return Callees.at(&Node);
  }
  bool IsPublic(const lex::Node &Node) const;
  const std::vector<ExternalFunction> &GetExternalFunctions() const {
    return ExternalFunctions;
  }
  const CWrapper *GetCWrapper(const lex::Node &Node) const;
  const std::vector<CWrapper> &GetCWrappers() const { return CWrappers; }
};
} // namespace kelyra::sema
