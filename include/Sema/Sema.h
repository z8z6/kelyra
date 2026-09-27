#pragma once

#include "Lexer/Lexer.h"
#include "Sema/ConstEval.h"
#include "Sema/Reflection.h"
#include "Sema/Type.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kelyra::sema {
struct ModuleInput {
  const lex::Node *Ast;
  bool IsEntry = false;
  bool IsExternal = false;
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
  struct Field {
    std::string Name;
    Type Value;
    std::uint64_t OffsetBits = 0;
    bool Addressable = true;
  };
  std::vector<Field> Fields;
};

struct ExternalConstant {
  std::string Name;
  Type Value;
  std::string Integer;
};

struct CWrapper {
  std::string Name;
  std::string Source;
  std::vector<Type> Parameters;
  Type Return{BuiltinType::CInt, {}};
  std::vector<bool> ParametersByAddress;
  bool ReturnByAddress = false;
};

struct Warning {
  lex::Location Loc;
  std::string Message;
};

struct ForLoopInfo {
  Type Iterator{BuiltinType::Void, {}};
  Type Item{BuiltinType::Void, {}};
  Type Maybe{BuiltinType::Void, {}};
  std::string IterSymbol;
  std::string NextSymbol;
  std::string HasValueSymbol;
  std::string ValueSymbol;
};

struct ClassFieldInfo {
  const lex::Node *Node = nullptr;
  std::string Name;
  Type Value{BuiltinType::I32, {}};
  bool Public = false;
  std::uint64_t Offset = 0;
  unsigned LayoutIndex = 0;
  unsigned Alignment = 1;
};

struct ClassConstantInfo {
  const lex::Node *Node = nullptr;
  const lex::Node *Value = nullptr;
  std::string Name;
  Type ValueType{BuiltinType::I32, {}};
  bool Public = false;
};

struct ClassStaticFieldInfo {
  const lex::Node *Node = nullptr;
  std::string Name;
  Type Value{BuiltinType::I32, {}};
  std::string Symbol;
  bool Public = false;
};

struct ClassInfo {
  const lex::Node *Node = nullptr;
  std::string Module;
  std::string Name;
  std::string QualifiedName;
  std::string BaseName;
  std::vector<std::string> Interfaces;
  std::vector<std::string> InterfaceMethods;
  std::unordered_map<std::string, std::size_t> VirtualSlots;
  std::unordered_map<std::string, std::pair<std::string, std::size_t>>
      OverrideSlots;
  std::size_t OwnFieldStart = 0;
  std::size_t UserFieldCount = 0;
  std::vector<ClassFieldInfo> Fields;
  std::vector<ClassStaticFieldInfo> StaticFields;
  std::vector<ClassConstantInfo> Constants;
  bool IsInterface = false;
  bool RawStorage = false;
  bool Final = false;
  const lex::Node *Constructor = nullptr;
  const lex::Node *Destructor = nullptr;
  const lex::Node *Copy = nullptr;
  const lex::Node *Move = nullptr;
  std::string ConstructorSymbol;
  std::string DestructorSymbol;
  std::string CopySymbol;
  std::string MoveSymbol;
  bool Public = false;
  // True when the class can be constructed with no arguments, either through
  // an explicit zero-parameter init or the generated default constructor.
  bool DefaultConstructible = false;
  bool CLayout = false;
  std::uint64_t Size = 0;
  unsigned Alignment = 1;
};

struct EnumVariantInfo {
  std::string Name;
  std::string Value;
  const lex::Node *Node = nullptr;
};

struct EnumInfo {
  const lex::Node *Node = nullptr;
  std::string Module;
  std::string QualifiedName;
  Type Underlying{BuiltinType::I32, {}};
  std::vector<EnumVariantInfo> Variants;
  bool Public = false;
  bool HasZero = false;
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
    AnnotationParameterTarget = 1u << 7,
    AnnotationModule = 1u << 8,
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
    unsigned Targets = AnnotationModule | AnnotationFunction | AnnotationClass |
                       AnnotationDeclaration | AnnotationField |
                       AnnotationMethod | AnnotationConstructor |
                       AnnotationDestructor | AnnotationParameterTarget;
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
    bool Abstract = false;
    bool Virtual = false;
    bool Override = false;
    bool Static = false;
    std::string OwnerClass;
    const ExternalFunction *External = nullptr;
  };

  struct TypeDeclarationInfo {
    const lex::Node *Node = nullptr;
    std::string Module;
    std::string QualifiedName;
    std::string OwnerClass;
    bool Public = false;
    unsigned State = 0;
    std::optional<Type> Resolved;
  };

  std::vector<lex::Diagnostic> Diagnostics;
  std::vector<Warning> Warnings;
  ReflectionDatabase Reflection;
  std::unordered_map<const lex::Node *, Type> Types;
  std::unordered_map<std::string, FunctionInfo> Functions;
  std::unordered_map<std::string, ClassInfo> Classes;
  std::unordered_map<std::string, EnumInfo> Enums;
  std::unordered_map<std::string, TypeDeclarationInfo> TypeDeclarations;
  std::unordered_map<std::string, AnnotationInfo> AnnotationDeclarations;
  std::unordered_set<std::string> MetaModules;
  std::unordered_set<const lex::Node *> MetaDeclarations;
  bool CurrentMetaContext = false;
  bool MetaRestrictionsReady = false;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      RuntimeDependencies;
  std::optional<lex::ParseResult> BuiltinAnnotations;
  std::optional<lex::ParseResult> BuiltinMeta;
  std::optional<lex::ParseResult> BuiltinCTypes;
  const lex::Node *MetaModule = nullptr;
  std::vector<const lex::Node *> EntrypointCandidates;
  std::unordered_map<const lex::Node *, std::vector<AnnotationInstance>>
      AnnotationInstances;
  std::unordered_map<const lex::Node *, std::string> Symbols;
  std::unordered_map<const lex::Node *, std::string> Callees;
  std::unordered_map<const lex::Node *, std::size_t> CWrapperCalls;
  std::unordered_map<const lex::Node *, std::string> ConstructorCalls;
  std::unordered_map<const lex::Node *, std::string> BaseConstructorCalls;
  std::unordered_map<const lex::Node *, std::unique_ptr<lex::Node>>
      InlineConstructors;
  std::unordered_set<const lex::Node *> ForwardTemporaries;
  std::unordered_map<const lex::Node *, std::pair<std::string, std::size_t>>
      VirtualCalls;
  std::unordered_map<const lex::Node *, std::string> InterfaceConversions;
  std::unordered_map<const lex::Node *, std::pair<std::string, std::size_t>>
      InterfaceCalls;
  std::unordered_map<const lex::Node *, std::pair<std::string, std::size_t>>
      FieldReferences;
  std::unordered_map<const lex::Node *, std::pair<std::string, std::size_t>>
      StaticFieldReferences;
  std::unordered_map<const lex::Node *, const lex::Node *> ConstantReferences;
  std::unordered_map<const lex::Node *, std::unique_ptr<lex::Node>>
      EvaluatedConstants;
  std::unordered_map<const lex::Node *, ExternalType::Field>
      ExternalFieldReferences;
  std::unordered_map<const lex::Node *, ExternalConstant>
      ExternalConstantReferences;
  std::unordered_map<const lex::Node *, EnumVariantInfo> EnumVariantReferences;
  std::unordered_map<const lex::Node *, std::string> MatchPatternValues;
  std::unordered_set<const lex::Node *> MethodCalls;
  std::unordered_map<const lex::Node *, std::string> FunctionValues;
  std::unordered_set<const lex::Node *> IndirectCalls;
  std::unordered_map<const lex::Node *, const lex::Node *> WhenBranches;
  std::unordered_map<const lex::Node *, ForLoopInfo> ForLoops;
  std::unordered_map<std::string, std::unordered_set<std::string>> Imports;
  std::unordered_map<std::string, Type> ExternalTypes;
  std::unordered_map<std::string, std::vector<ExternalType::Field>>
      ExternalFields;
  std::unordered_map<std::string, ExternalConstant> ExternalConstants;
  std::vector<ExternalFunction> ExternalFunctions;
  std::vector<CWrapper> CWrappers;
  std::vector<std::unordered_map<std::string, Type>> Scopes;
  std::vector<std::unordered_map<std::string, Type>> LocalTypeScopes;
  unsigned CurrentLoopDepth = 0;
  std::optional<Type> ReturnType;
  std::string CurrentModule;
  std::string CurrentClass;
  // Distinguishes generated C thunk symbols from another compilation unit so a
  // linked library and its consumer never collide.
  std::string SymbolPrefix = "unit";
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
  void Warn(const lex::Node &Node, std::string Message);
  void WarnIfDeprecated(const lex::Node &Use, const lex::Node *Declaration);
  void GenerateSymbolPrefix(const std::vector<ModuleInput> &Modules);
  void RegisterAnnotation(const lex::Node &Declaration,
                          std::string_view Module);
  void CheckAnnotationDefinition(const lex::Node &Declaration);
  void CheckAnnotations(const lex::Node &Target);
  const AnnotationInfo *ResolveAnnotation(const lex::Node &Annotation,
                                          bool &Ambiguous) const;
  std::optional<AnnotationValue>
  ParseAnnotationValue(const lex::Node &Expression,
                       std::string_view ExpectedType);
  MetaId RegisterMetaDeclaration(const lex::Node &Node, MetaKind Kind,
                                 std::string_view Module, bool Public);
  MetaId GetOrCreateMetaType(const Type &Type);
  std::optional<Type> CheckType(const lex::Node &Node);
  bool ContainsMetaType(const Type &Value) const;
  std::optional<Type> ResolveTypeDeclaration(TypeDeclarationInfo &Declaration);
  std::optional<Type> FinishExpression(const lex::Node &Expression, Type Result,
                                       std::optional<Type> Expected);
  std::optional<std::string>
  EvaluateIntegerConstant(const lex::Node &Expression);
  std::optional<ConstValue> EvaluateConstant(const lex::Node &Expression);
  bool CanZeroInitialize(Type Value) const;
  std::optional<Type> CheckNameExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  std::optional<Type> CheckLiteralExpression(const lex::Node &Expression,
                                             std::optional<Type> Expected,
                                             bool Negated);
  std::optional<Type> CheckGroupExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckIndexExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckSliceExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckMemberExpression(const lex::Node &Expression,
                                            std::optional<Type> Expected, bool);
  std::optional<Type> CheckCallExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  std::optional<Type> CheckUnaryExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckBinaryExpression(const lex::Node &Expression,
                                            std::optional<Type> Expected, bool);
  std::optional<Type> CheckCastExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  std::optional<Type> CheckMetaExpression(const lex::Node &Expression,
                                          std::optional<Type> Expected, bool);
  std::optional<Type> CheckMetaBlockExpression(const lex::Node &Expression,
                                               std::optional<Type> Expected,
                                               bool);
  std::optional<Type> CheckBlockExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  std::optional<Type> CheckMatchExpression(const lex::Node &Expression,
                                           std::optional<Type> Expected, bool);
  void CheckFunction(const lex::Node &Function);
  void CheckClassMember(const lex::Node &Member, const ClassInfo &Class);
  bool CheckForwardConstructor(const lex::Node &Expression,
                               const ClassInfo &Class);
  void CheckTransferAccess(const Type &Value, bool Move, const lex::Node &Site);
  void CheckBlock(const lex::Node &Block, unsigned LoopDepth = 0);
  void CheckBlockStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckAliasStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckLetStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckAssignStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckExpressionStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckReturnStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckIfStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckWhenStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckWhileStatement(const lex::Node &Statement, unsigned LoopDepth);
  void CheckForStatement(const lex::Node &Statement, unsigned LoopDepth);
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
  std::optional<ConstValue>
  EvaluateMetaIntrinsic(const lex::Node &Call,
                        const std::vector<ConstValue> &Arguments);
  bool AlwaysReturns(const lex::Node &Node) const;

public:
  bool Check(const lex::Node &Module);
  bool CheckModules(
      const std::vector<ModuleInput> &Modules,
      const std::vector<ExternalFunction> &ExternalDeclarations = {},
      const std::vector<ExternalType> &ExternalTypeDeclarations = {},
      const std::vector<ExternalConstant> &ExternalConstantDeclarations = {});
  bool CheckEntrypoint(const lex::Node &Module);
  bool IsMetaModule(std::string_view Name) const {
    return MetaModules.contains(std::string(Name));
  }
  bool IsMetaDeclaration(const lex::Node &Node) const {
    return MetaDeclarations.contains(&Node);
  }
  const std::unordered_set<std::string> &
  GetRuntimeDependencies(std::string_view Module) const {
    static const std::unordered_set<std::string> Empty;
    const auto It = RuntimeDependencies.find(std::string(Module));
    return It == RuntimeDependencies.end() ? Empty : It->second;
  }
  const std::vector<lex::Diagnostic> &GetDiagnostics() const {
    return Diagnostics;
  }
  const std::vector<Warning> &GetWarnings() const { return Warnings; }
  const Type &GetType(const lex::Node &Node) const { return Types.at(&Node); }
  const ForLoopInfo &GetForLoop(const lex::Node &Node) const {
    return ForLoops.at(&Node);
  }
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
  const EnumInfo *GetEnum(std::string_view Name) const;
  const ClassInfo *GetClass(const Type &Value) const;
  const ClassFieldInfo *GetField(const lex::Node &Node) const;
  const ClassStaticFieldInfo *GetStaticField(const lex::Node &Node) const;
  const lex::Node *GetConstant(const lex::Node &Node) const {
    const auto It = ConstantReferences.find(&Node);
    return It == ConstantReferences.end() ? nullptr : It->second;
  }
  const ExternalType::Field *GetExternalField(const lex::Node &Node) const {
    const auto It = ExternalFieldReferences.find(&Node);
    return It == ExternalFieldReferences.end() ? nullptr : &It->second;
  }
  const ExternalConstant *GetExternalConstant(const lex::Node &Node) const {
    const auto It = ExternalConstantReferences.find(&Node);
    return It == ExternalConstantReferences.end() ? nullptr : &It->second;
  }
  const EnumVariantInfo *GetEnumVariant(const lex::Node &Node) const {
    const auto It = EnumVariantReferences.find(&Node);
    return It == EnumVariantReferences.end() ? nullptr : &It->second;
  }
  const std::string *GetMatchPatternValue(const lex::Node &Node) const {
    const auto It = MatchPatternValues.find(&Node);
    return It == MatchPatternValues.end() ? nullptr : &It->second;
  }
  std::size_t GetFieldIndex(const lex::Node &Node) const;
  const ClassInfo *GetFieldOwner(const lex::Node &Node) const {
    const auto It = FieldReferences.find(&Node);
    return It == FieldReferences.end() ? nullptr : GetClass(It->second.first);
  }
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
  const lex::Node *GetInlineConstructor(const lex::Node &Node) const {
    const auto It = InlineConstructors.find(&Node);
    return It == InlineConstructors.end() ? nullptr : It->second.get();
  }
  bool IsForwardTemporary(const lex::Node &Node) const {
    return ForwardTemporaries.contains(&Node);
  }
  const ClassInfo *GetBaseConstructorCall(const lex::Node &Node) const {
    const auto It = BaseConstructorCalls.find(&Node);
    return It == BaseConstructorCalls.end() ? nullptr : GetClass(It->second);
  }
  const std::pair<std::string, std::size_t> *
  GetVirtualCall(const lex::Node &Node) const {
    const auto It = VirtualCalls.find(&Node);
    return It == VirtualCalls.end() ? nullptr : &It->second;
  }
  const std::string *GetInterfaceConversion(const lex::Node &Node) const {
    const auto It = InterfaceConversions.find(&Node);
    return It == InterfaceConversions.end() ? nullptr : &It->second;
  }
  const std::pair<std::string, std::size_t> *
  GetInterfaceCall(const lex::Node &Node) const {
    const auto It = InterfaceCalls.find(&Node);
    return It == InterfaceCalls.end() ? nullptr : &It->second;
  }
  bool IsClassTemporary(const lex::Node &Node) const;
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
