#pragma once

#include "Lexer/Lexer.h"
#include "Sema/Reflection.h"
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

struct ClassFieldInfo {
  const lex::Node *Node = nullptr;
  std::string Name;
  Type Value{BuiltinType::I32, {}};
  bool Public = false;
  std::uint64_t Offset = 0;
  unsigned LayoutIndex = 0;
};

struct ClassInfo {
  const lex::Node *Node = nullptr;
  std::string Module;
  std::string Name;
  std::string QualifiedName;
  std::vector<ClassFieldInfo> Fields;
  const lex::Node *Constructor = nullptr;
  const lex::Node *Destructor = nullptr;
  std::string ConstructorSymbol;
  std::string DestructorSymbol;
  bool Public = false;
  std::uint64_t Size = 0;
  unsigned Alignment = 1;
};

class Sema {
  enum AnnotationTarget : unsigned {
    AnnotationFunction = 1u << 0,
    AnnotationClass = 1u << 1,
    AnnotationDeclaration = 1u << 2,
    AnnotationField = 1u << 3,
    AnnotationMethod = 1u << 4,
    AnnotationConstructor = 1u << 5,
    AnnotationDestructor = 1u << 6,
  };

  enum class AnnotationRetention { Source, Compile };

  struct AnnotationParameter {
    std::string Name;
    std::string TypeName;
    const lex::Node *Default = nullptr;
  };

  struct AnnotationInfo {
    const lex::Node *Node = nullptr;
    std::string Module;
    std::vector<AnnotationParameter> Parameters;
    unsigned Targets = AnnotationFunction | AnnotationClass |
                       AnnotationDeclaration | AnnotationField |
                       AnnotationMethod | AnnotationConstructor |
                       AnnotationDestructor;
    AnnotationRetention Retention = AnnotationRetention::Compile;
    bool Public = false;
    bool Repeatable = false;
  };

  struct FunctionInfo {
    const lex::Node *Node = nullptr;
    std::string Module;
    std::string Symbol;
    std::vector<Type> Parameters;
    Type Return{BuiltinType::Void, {}};
    bool Public = false;
    bool Variadic = false;
    std::string OwnerClass;
    const ExternalFunction *External = nullptr;
  };

  std::vector<lex::Diagnostic> Diagnostics;
  ReflectionDatabase Reflection;
  std::unordered_map<const lex::Node *, Type> Types;
  std::unordered_map<std::string, FunctionInfo> Functions;
  std::unordered_map<std::string, ClassInfo> Classes;
  std::unordered_map<std::string, AnnotationInfo> AnnotationDeclarations;
  std::unordered_map<const lex::Node *, std::vector<AnnotationInstance>>
      AnnotationInstances;
  std::unordered_map<const lex::Node *, std::string> Symbols;
  std::unordered_map<const lex::Node *, std::string> Callees;
  std::unordered_map<const lex::Node *, std::size_t> CWrapperCalls;
  std::unordered_map<const lex::Node *, std::string> ConstructorCalls;
  std::unordered_map<const lex::Node *, std::pair<std::string, std::size_t>>
      FieldReferences;
  std::unordered_set<const lex::Node *> MethodCalls;
  std::unordered_map<const lex::Node *, std::string> FunctionValues;
  std::unordered_set<const lex::Node *> IndirectCalls;
  std::unordered_map<const lex::Node *, const lex::Node *> WhenBranches;
  std::unordered_map<std::string, std::unordered_set<std::string>> Imports;
  std::unordered_map<std::string, Type> ExternalTypes;
  std::vector<ExternalFunction> ExternalFunctions;
  std::vector<CWrapper> CWrappers;
  std::vector<std::unordered_map<std::string, Type>> Scopes;
  std::optional<Type> ReturnType;
  std::string CurrentModule;
  std::string CurrentClass;
  const lex::Node *CurrentConstructor = nullptr;
  const lex::Node *ConstructionContext = nullptr;
  const lex::Node *InitializingTarget = nullptr;
  std::size_t InitializedFields = 0;
  bool CheckingFieldBase = false;
  bool InDestructor = false;

  void CheckClassLayouts();
  std::optional<Type> CheckFunctionType(const lex::Node &Node);
  std::optional<Type> CheckFunctionValue(const lex::Node &Node,
                                         std::optional<Type> Expected);
  std::optional<Type> CheckIndirectCall(const lex::Node &Node,
                                        std::optional<Type> Expected);

  void Error(const lex::Node &Node, lex::DiagnosticKind Kind);
  void RegisterAnnotation(const lex::Node &Declaration,
                          std::string_view Module);
  void CheckAnnotationDefinition(const lex::Node &Declaration);
  void CheckAnnotations(const lex::Node &Target);
  const AnnotationInfo *ResolveAnnotation(const lex::Node &Annotation) const;
  std::optional<AnnotationValue>
  ParseAnnotationValue(const lex::Node &Expression,
                       std::string_view ExpectedType);
  MetaId RegisterMetaDeclaration(const lex::Node &Node, MetaKind Kind,
                                 std::string_view Module, bool Public);
  MetaId GetOrCreateMetaType(const Type &Type);
  std::optional<Type> CheckType(const lex::Node &Node);
  std::optional<Type> FinishExpression(const lex::Node &Expression, Type Result,
                                       std::optional<Type> Expected);
  std::optional<Type> CheckNameExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  std::optional<Type> CheckLiteralExpression(const lex::Node &Expression,
                                             std::optional<Type> Expected,
                                             bool Negated);
  std::optional<Type> CheckGroupExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckIndexExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckMemberExpression(const lex::Node &Expression,
                                            std::optional<Type> Expected, bool);
  std::optional<Type> CheckCallExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  std::optional<Type> CheckUnaryExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckBinaryExpression(const lex::Node &Expression,
                                            std::optional<Type> Expected, bool);
  std::optional<Type> CheckMetaExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  void CheckFunction(const lex::Node &Function);
  void CheckClassMember(const lex::Node &Member, const ClassInfo &Class);
  void CheckBlock(const lex::Node &Block, unsigned LoopDepth = 0);
  void CheckBlockStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckLetStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckAssignStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckExpressionStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckReturnStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckIfStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckWhenStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckWhileStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckLoopControlStatement(const lex::Node &Statement,
                                 unsigned LoopDepth);
  void CheckAsmStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckStatement(const lex::Node &Statement, unsigned LoopDepth);
  std::optional<Type>
  CheckExpression(const lex::Node &Expression,
                  std::optional<Type> Expected = std::nullopt,
                  bool Negated = false);
  const Type *FindName(std::string_view Name) const;
  std::optional<MetaId> ResolveMetaTarget(const lex::Node &Target);
  std::optional<bool> EvaluateWhen(const lex::Node &Expression);
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
  const ClassInfo *GetClass(std::string_view Name) const;
  const ClassInfo *GetClass(const Type &Value) const;
  const ClassFieldInfo *GetField(const lex::Node &Node) const;
  std::size_t GetFieldIndex(const lex::Node &Node) const;
  bool IsMethodCall(const lex::Node &Node) const {
    return MethodCalls.contains(&Node);
  }
  const std::string *GetFunctionValue(const lex::Node &Node) const {
    const auto It = FunctionValues.find(&Node);
    return It == FunctionValues.end() ? nullptr : &It->second;
  }
  bool IsIndirectCall(const lex::Node &Node) const {
    return IndirectCalls.contains(&Node);
  }
  const ClassInfo *GetConstructorCall(const lex::Node &Node) const;
  const std::unordered_map<std::string, ClassInfo> &GetClasses() const {
    return Classes;
  }
  const std::vector<AnnotationInstance> &
  GetAnnotations(const lex::Node &Node) const;
  const lex::Node *GetWhenBranch(const lex::Node &Node) const {
    const auto It = WhenBranches.find(&Node);
    return It == WhenBranches.end() ? nullptr : It->second;
  }
  const ReflectionDatabase &GetReflection() const { return Reflection; }
};
} // namespace kelyra::sema
