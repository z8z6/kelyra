#include "Sema/Sema.h"
#include "BuiltinAnnotations.h"
#include "BuiltinCTypes.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <sstream>

using namespace kelyra;

namespace {
bool ValidForwardConstructor(const lex::Node &Constructor) {
  using K = lex::TokenKind;
  const lex::Node *Generic = nullptr;
  const lex::Node *Parameter = nullptr;
  for (const auto &Child : Constructor.children) {
    if (Child->kind == K::ast_generic_pack) {
      if (Generic)
        return false;
      Generic = Child.get();
    } else if (Child->kind == K::ast_parameter_pack) {
      if (Parameter)
        return false;
      Parameter = Child.get();
    } else if (Child->kind == K::ast_generic_parameter ||
               Child->kind == K::ast_parameter)
      return false;
  }
  return Generic && Parameter && !Parameter->children.empty() &&
         Parameter->children.back()->kind == K::ast_type &&
         Parameter->children.back()->text == Generic->text &&
         std::any_of(Parameter->children.begin(), Parameter->children.end(),
                     [](const auto &Child) {
                       return Child->kind == K::ast_annotation &&
                              IsBuiltinAnnotation(Child->text, "forward");
                     });
}

bool IsConstantExpression(const lex::Node &Expression) {
  using K = lex::TokenKind;
  if (Expression.kind == K::ast_literal)
    return true;
  if (Expression.kind == K::ast_group && Expression.children.size() == 1)
    return IsConstantExpression(*Expression.children.front());
  if (Expression.kind == K::ast_unary && Expression.children.size() == 1 &&
      (Expression.text == "+" || Expression.text == "-" ||
       Expression.text == "!"))
    return IsConstantExpression(*Expression.children.front());
  if (Expression.kind == K::ast_binary && Expression.children.size() == 2)
    return IsConstantExpression(*Expression.children[0]) &&
           IsConstantExpression(*Expression.children[1]);
  return false;
}

std::string ModuleName(const lex::Node &Module) {
  for (const auto &Child : Module.children)
    if (Child->kind == lex::TokenKind::ast_module_decl)
      return Child->text;
  return {};
}

std::string Mangle(std::string_view Module, std::string_view Name,
                   bool IsEntrypoint) {
  if (IsEntrypoint)
    return "main";
  if (Module.empty())
    return std::string(Name);
  std::ostringstream Result;
  Result << "_K";
  std::size_t Start = 0;
  while (Start < Module.size()) {
    const auto End = Module.find('.', Start);
    const auto Part = Module.substr(Start, End == std::string_view::npos
                                               ? Module.size() - Start
                                               : End - Start);
    Result << Part.size() << Part;
    if (End == std::string_view::npos)
      break;
    Start = End + 1;
  }
  Result << 'F' << Name.size() << Name;
  return Result.str();
}

std::string MetaTypeName(const sema::Type &Type) {
  if (Type.IsPointer())
    return "*" + MetaTypeName(Type.Pointee());
  if (Type.IsArray())
    return "[" + std::to_string(Type.ArrayLength()) + "]" +
           MetaTypeName(Type.Indexed());
  if (Type.Element == sema::BuiltinType::Function) {
    std::string Name = "fn(";
    for (std::size_t I = 0; I < Type.Parameters.size(); ++I) {
      if (I)
        Name += ", ";
      Name += MetaTypeName(Type.Parameters[I]);
    }
    Name += ") -> " + MetaTypeName(Type.Results.front());
    return Name;
  }
  if (Type.IsResults()) {
    std::string Name = "(";
    for (const auto &Result : Type.Results) {
      if (Name.size() > 1)
        Name += ", ";
      Name += MetaTypeName(Result);
    }
    return Name + ")";
  }
  std::string Result =
      Type.Element == sema::BuiltinType::Class ? Type.ClassName
      : Type.IsVoid()                          ? "void"
      : Type.CName.empty()
          ? std::string(sema::GetBuiltinTypeInfo(Type.Element).Name)
          : Type.CName;
  return Result;
}

} // namespace

void sema::Sema::Error(const lex::Node &Node, lex::DiagnosticKind Kind) {
  Diagnostics.push_back({Kind, Node.Loc});
}

void sema::Sema::Warn(const lex::Node &Node, std::string Message) {
  Warnings.push_back({Node.Loc, std::move(Message)});
}

void sema::Sema::WarnIfDeprecated(const lex::Node &Use,
                                  const lex::Node *Declaration) {
  if (!Declaration)
    return;
  for (const auto &Annotation : GetAnnotations(*Declaration)) {
    if (Annotation.Name != "std.annotation.deprecated")
      continue;
    std::string Message =
        "use of deprecated function '" + Declaration->text + "'";
    if (!Annotation.Arguments.empty() &&
        Annotation.Arguments.front().Value.Text.size() > 2) {
      auto Detail = Annotation.Arguments.front().Value.Text;
      if (Detail.front() == '"' && Detail.back() == '"')
        Detail = Detail.substr(1, Detail.size() - 2);
      Message += ": " + Detail;
    }
    Warn(Use, std::move(Message));
  }
}

sema::MetaId sema::Sema::RegisterMetaDeclaration(const lex::Node &Node,
                                                 MetaKind Kind,
                                                 std::string_view Module,
                                                 bool Public) {
  MetaDeclaration Declaration;
  Declaration.Kind = Kind;
  Declaration.Name = Node.text;
  Declaration.QualifiedName =
      Module.empty() ? Node.text : std::string(Module) + "." + Node.text;
  Declaration.Public = Public;
  Declaration.Loc = Node.Loc;
  const auto ModuleId = Reflection.Find(Module, MetaKind::Module);
  if (ModuleId)
    Declaration.Module = *ModuleId;
  const auto Id = Reflection.Add(&Node, std::move(Declaration));
  if (ModuleId && (Kind == MetaKind::Function || Kind == MetaKind::Class ||
                   Kind == MetaKind::Annotation))
    Reflection.Records[*ModuleId].Children.push_back(Id);
  return Id;
}

sema::MetaId sema::Sema::GetOrCreateMetaType(const Type &Type) {
  const auto Name = MetaTypeName(Type);
  if (const auto Existing = Reflection.Find(Name, MetaKind::Type))
    return *Existing;
  MetaDeclaration Declaration;
  Declaration.Kind = MetaKind::Type;
  Declaration.Name = Name;
  Declaration.QualifiedName = Name;
  Declaration.TypeKind = Type.IsPointer() ? MetaTypeKind::Pointer
                         : Type.IsArray() ? MetaTypeKind::Array
                         : (Type.IsRecord() || Type.IsClass())
                             ? MetaTypeKind::Record
                             : MetaTypeKind::Builtin;
  Declaration.BitWidth = GetBitWidth(Type);
  Declaration.Alignment = GetAlignment(Type);
  if (Type.IsClass()) {
    Declaration.BitWidth = GetClass(Type)->Size * 8;
    Declaration.Alignment = GetClass(Type)->Alignment;
  }
  Declaration.PointerDepth = Type.PointerDepth;
  Declaration.Dimensions = Type.Dimensions;
  if (Type.IsFunction() && !Type.IsArray()) {
    Declaration.TypeKind = MetaTypeKind::Function;
    Declaration.Type = GetOrCreateMetaType(Type.Results.front());
    for (const auto &Parameter : Type.Parameters)
      Declaration.Children.push_back(GetOrCreateMetaType(Parameter));
  }
  if (Type.IsResults()) {
    Declaration.TypeKind = MetaTypeKind::Results;
    for (const auto &Result : Type.Results)
      Declaration.Children.push_back(GetOrCreateMetaType(Result));
  }
  if (Type.IsPointer()) {
    Declaration.Type = GetOrCreateMetaType(Type.Pointee());
  } else if (Type.IsArray()) {
    Declaration.Type = GetOrCreateMetaType(Type.Indexed());
  }
  return Reflection.Add(nullptr, std::move(Declaration));
}

const sema::Type *sema::Sema::FindName(std::string_view Name) const {
  for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope) {
    const auto It = Scope->find(std::string(Name));
    if (It != Scope->end())
      return &It->second;
  }
  return nullptr;
}

const sema::ClassInfo *sema::Sema::GetClass(std::string_view Name) const {
  const auto It = Classes.find(std::string(Name));
  return It == Classes.end() ? nullptr : &It->second;
}

const sema::ClassInfo *sema::Sema::GetClass(const Type &Value) const {
  return Value.Element == BuiltinType::Class ? GetClass(Value.ClassName)
                                             : nullptr;
}

const sema::ClassFieldInfo *sema::Sema::GetField(const lex::Node &Node) const {
  const auto It = FieldReferences.find(&Node);
  if (It == FieldReferences.end())
    return nullptr;
  const auto *Class = GetClass(It->second.first);
  return Class ? &Class->Fields.at(It->second.second) : nullptr;
}

const sema::ClassStaticFieldInfo *
sema::Sema::GetStaticField(const lex::Node &Node) const {
  const auto It = StaticFieldReferences.find(&Node);
  if (It == StaticFieldReferences.end())
    return nullptr;
  const auto *Class = GetClass(It->second.first);
  return Class ? &Class->StaticFields.at(It->second.second) : nullptr;
}

std::size_t sema::Sema::GetFieldIndex(const lex::Node &Node) const {
  return FieldReferences.at(&Node).second;
}

const sema::ClassInfo *
sema::Sema::GetConstructorCall(const lex::Node &Node) const {
  const auto It = ConstructorCalls.find(&Node);
  return It == ConstructorCalls.end() ? nullptr : GetClass(It->second);
}

bool sema::Sema::IsClassTemporary(const lex::Node &Node) const {
  if (ForwardTemporaries.contains(&Node))
    return true;
  if (GetConstructorCall(Node))
    return true;
  if (Node.kind == lex::TokenKind::ast_group && Node.children.size() == 1)
    return IsClassTemporary(*Node.children.front());
  const auto Type = Types.find(&Node);
  return Node.kind == lex::TokenKind::ast_call && Type != Types.end() &&
         Type->second.IsClass();
}

std::optional<sema::Type>
sema::Sema::ResolveTypeDeclaration(TypeDeclarationInfo &Declaration) {
  if (Declaration.State == 2)
    return Declaration.Resolved;
  if (Declaration.State == 1) {
    Error(*Declaration.Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  Declaration.State = 1;
  const auto PreviousModule = CurrentModule;
  CurrentModule = Declaration.Module;
  auto Result = CheckType(*Declaration.Node->children.back());
  CurrentModule = PreviousModule;
  Declaration.Resolved = Result;
  Declaration.State = 2;
  return Result;
}

std::optional<sema::Type> sema::Sema::CheckType(const lex::Node &Node) {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_function_type)
    return CheckFunctionType(Node);
  if (Node.kind == K::ast_result_types) {
    Type Result{BuiltinType::Results, {}};
    for (const auto &Child : Node.children) {
      auto Value = CheckType(*Child);
      if (!Value)
        return std::nullopt;
      if (Value->IsVoid() || Value->IsClass() || Value->IsRecord() ||
          Value->IsArray() || Value->IsResults() || GetBitWidth(*Value) > 128) {
        Error(*Child, lex::DiagnosticKind::UnsupportedType);
        return std::nullopt;
      }
      Result.Results.push_back(*Value);
    }
    Types[&Node] = Result;
    return Result;
  }
  if (Node.kind == K::ast_type) {
    const auto Element = ParseBuiltinType(Node.text);
    const auto External = ExternalTypes.find(Node.text);
    std::string ClassName = Node.text;
    const auto &AccessModule =
        Node.GenericArgument ? Node.GenericOriginModule : CurrentModule;
    if (Element && Node.text.starts_with("__c_") && AccessModule != "c") {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    if (!Classes.contains(ClassName) &&
        Node.text.find('.') == std::string::npos)
      ClassName =
          AccessModule.empty() ? Node.text : AccessModule + "." + Node.text;
    const auto Class = Classes.find(ClassName);
    const auto Declaration = TypeDeclarations.find(ClassName);
    if (!Element && External == ExternalTypes.end() && Class == Classes.end() &&
        Declaration == TypeDeclarations.end()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Type Result;
    if (Element)
      Result = Type{*Element, {}};
    else if (External != ExternalTypes.end())
      Result = External->second;
    else if (Declaration != TypeDeclarations.end()) {
      if (Declaration->second.Module != AccessModule) {
        const auto Import = Imports.find(AccessModule);
        const bool Imported =
            Declaration->second.Module == "c" ||
            (Import != Imports.end() &&
             (Import->second.contains(Declaration->second.Module) ||
              Import->second.contains(Declaration->second.Module + ".*")));
        if (!Declaration->second.Public || !Imported) {
          Error(Node, lex::DiagnosticKind::PrivateDeclaration);
          return std::nullopt;
        }
      }
      auto Alias = ResolveTypeDeclaration(Declaration->second);
      if (!Alias)
        return std::nullopt;
      Result = *Alias;
    } else {
      if (Class->second.Module != AccessModule) {
        const auto Import = Imports.find(AccessModule);
        if (!Class->second.Public || Import == Imports.end() ||
            (!Import->second.contains(Class->second.Module) &&
             !Import->second.contains(Class->second.Module + ".*"))) {
          Error(Node, lex::DiagnosticKind::PrivateDeclaration);
          return std::nullopt;
        }
      }
      Result = Type{BuiltinType::Class, {}};
      Result.ClassName = ClassName;
    }
    if (Node.GenericArgument) {
      Result.GenericArgument = true;
      Result.GenericOriginModule = Node.GenericOriginModule;
    }
    Types[&Node] = Result;
    return Result;
  }
  if (Node.kind == K::ast_pointer_type && Node.children.size() == 1) {
    auto Result = CheckType(*Node.children.front());
    if (!Result || Result->IsVoid() || Result->IsResults()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Result->AddPointer();
    if (Result->Element != BuiltinType::Class)
      Result->CSpelling = detail::CSpelling(*Result) + " *";
    Types[&Node] = *Result;
    return Result;
  }
  if (Node.kind != K::ast_array_type || Node.children.size() != 1) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  auto Result = CheckType(*Node.children.front());
  std::uint64_t Length = 0;
  const auto Parsed = std::from_chars(
      Node.text.data(), Node.text.data() + Node.text.size(), Length);
  if (!Result || Result->IsVoid() || Result->IsResults() ||
      Parsed.ec != std::errc() || Length == 0) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  if (Result->IsClass()) {
    Error(Node, lex::DiagnosticKind::ClassValueOperation);
    return std::nullopt;
  }
  if (!Result->IsRecord() && GetBitWidth(*Result) > 128) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  Result->AddArray(Length);
  Types[&Node] = *Result;
  return Result;
}

void sema::Sema::CheckBlock(const lex::Node &Block, unsigned LoopDepth) {
  Scopes.emplace_back();
  for (const auto &Statement : Block.children)
    CheckStatement(*Statement, LoopDepth);
  Scopes.pop_back();
}

bool sema::Sema::AlwaysReturns(const lex::Node &Node) const {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_return)
    return true;
  if (Node.kind == K::ast_block) {
    for (const auto &Child : Node.children)
      if (AlwaysReturns(*Child))
        return true;
    return false;
  }
  if (Node.kind == K::ast_when) {
    const auto *Branch = GetWhenBranch(Node);
    return Branch && AlwaysReturns(*Branch);
  }
  return Node.kind == K::ast_if && Node.children.size() == 3 &&
         AlwaysReturns(*Node.children[1]) && AlwaysReturns(*Node.children[2]);
}

void sema::Sema::CheckFunction(const lex::Node &Function) {
  using K = lex::TokenKind;
  Scopes.clear();
  Scopes.emplace_back();
  const lex::Node *ReturnTypeNode = nullptr;
  const lex::Node *Body = nullptr;
  for (const auto &Child : Function.children) {
    if (Child->kind == K::ast_parameter) {
      if (Child->children.empty() ||
          !detail::IsTypeNode(Child->children.back()->kind))
        continue;
      auto ParameterType = CheckType(*Child->children.back());
      if (!ParameterType)
        continue;
      Types[Child.get()] = *ParameterType;
      if (!Scopes.back().emplace(Child->text, *ParameterType).second)
        Error(*Child, lex::DiagnosticKind::DuplicateParameter);
    } else if (detail::IsTypeNode(Child->kind)) {
      ReturnTypeNode = Child.get();
    } else if (Child->kind == K::ast_block) {
      Body = Child.get();
    }
  }
  ReturnType = ReturnTypeNode ? CheckType(*ReturnTypeNode) : std::nullopt;
  if (ReturnType && ReturnType->IsVoid())
    ReturnType.reset();
  if (!Body) {
    Error(Function, lex::DiagnosticKind::MissingReturn);
    return;
  }
  CheckBlock(*Body);
  if (ReturnType && !AlwaysReturns(*Body))
    Error(*Body, lex::DiagnosticKind::MissingReturn);
}

bool sema::Sema::Check(const lex::Node &Module) {
  return CheckModules({{&Module, true}});
}

bool sema::Sema::IsPublic(const lex::Node &Node) const {
  using K = lex::TokenKind;
  for (const auto &Child : Node.children)
    if (Child->kind == K::ast_public)
      return true;
  return false;
}

const sema::CWrapper *sema::Sema::GetCWrapper(const lex::Node &Node) const {
  const auto It = CWrapperCalls.find(&Node);
  return It == CWrapperCalls.end() ? nullptr : &CWrappers[It->second];
}

void sema::Sema::GenerateSymbolPrefix(const std::vector<ModuleInput> &Modules) {
  const lex::Node *Entry = nullptr;
  for (const auto &Input : Modules)
    if (Input.IsEntry) {
      Entry = Input.Ast;
      break;
    }
  if (!Entry && !Modules.empty())
    Entry = Modules.front().Ast;
  std::uint64_t Hash = 1469598103934665603ull;
  const auto Feed = [&Hash](std::string_view Text) {
    for (const char Character : Text) {
      Hash ^= static_cast<unsigned char>(Character);
      Hash *= 1099511628211ull;
    }
  };
  if (Entry) {
    Feed(Entry->Loc.File);
    Feed(ModuleName(*Entry));
  }
  SymbolPrefix = "u" + std::to_string(Hash);
}

bool sema::Sema::CheckModules(
    const std::vector<ModuleInput> &InputModules,
    const std::vector<ExternalFunction> &ExternalDeclarations,
    const std::vector<ExternalType> &ExternalTypeDeclarations) {
  using K = lex::TokenKind;
  std::vector<ModuleInput> Modules = InputModules;
  const bool HasBuiltinModule =
      std::any_of(Modules.begin(), Modules.end(), [](const auto &Input) {
        return ModuleName(*Input.Ast) == "std.annotation";
      });
  BuiltinAnnotations.reset();
  if (!HasBuiltinModule) {
    BuiltinAnnotations = lex::Lexer().parse(
        std::string(BuiltinAnnotationsSource), "std/annotation.kly");
    if (!BuiltinAnnotations->ok())
      return false;
    Modules.push_back({BuiltinAnnotations->root.get(), false, true});
  }
  const bool HasCModule =
      std::any_of(Modules.begin(), Modules.end(), [](const auto &Input) {
        return ModuleName(*Input.Ast) == "c";
      });
  BuiltinCTypes.reset();
  if (!HasCModule) {
    BuiltinCTypes =
        lex::Lexer().parse(std::string(BuiltinCTypesSource), "c.kly");
    if (!BuiltinCTypes->ok())
      return false;
    Modules.push_back({BuiltinCTypes->root.get(), false, true});
  }
  Diagnostics.clear();
  Warnings.clear();
  Reflection.Clear();
  Types.clear();
  Functions.clear();
  Classes.clear();
  TypeDeclarations.clear();
  AnnotationDeclarations.clear();
  AnnotationInstances.clear();
  EntrypointCandidates.clear();
  Symbols.clear();
  Callees.clear();
  CWrapperCalls.clear();
  ConstructorCalls.clear();
  BaseConstructorCalls.clear();
  InlineConstructors.clear();
  ForwardTemporaries.clear();
  VirtualCalls.clear();
  InterfaceConversions.clear();
  InterfaceCalls.clear();
  FieldReferences.clear();
  StaticFieldReferences.clear();
  MethodCalls.clear();
  FunctionValues.clear();
  IndirectCalls.clear();
  WhenBranches.clear();
  CWrappers.clear();
  Imports.clear();
  ExternalTypes.clear();
  CurrentClass.clear();
  CurrentConstructor = nullptr;
  ConstructionContext = nullptr;
  InitializingTarget = nullptr;
  InDestructor = false;
  ExternalFunctions = ExternalDeclarations;
  for (const auto &External : ExternalTypeDeclarations)
    ExternalTypes.emplace(External.Name, External.Value);
  GenerateSymbolPrefix(Modules);
  std::unordered_map<std::string, const lex::Node *> ModuleTable;

  for (const auto &Input : Modules) {
    const auto &Module = *Input.Ast;
    if (Module.kind != K::ast_module) {
      Error(Module, lex::DiagnosticKind::UnsupportedDeclaration);
      continue;
    }
    const auto Name = ModuleName(Module);
    if (!ModuleTable.emplace(Name, &Module).second) {
      Error(Module, lex::DiagnosticKind::DuplicateModule);
    } else {
      MetaDeclaration Declaration;
      Declaration.Kind = MetaKind::Module;
      Declaration.Name = Name;
      Declaration.QualifiedName = Name;
      Declaration.Public = true;
      Declaration.Loc = Module.Loc;
      Reflection.Add(&Module, std::move(Declaration));
    }
  }

  for (const auto &Input : Modules) {
    const auto Name = ModuleName(*Input.Ast);
    CurrentModule = Name;
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind == K::ast_import) {
        Imports[Name].insert(Child->text);
        const auto Imported =
            Child->text.ends_with(".*")
                ? Child->text.substr(0, Child->text.size() - 2)
                : Child->text;
        if (Imported != "c" && !ModuleTable.contains(Imported))
          Error(*Child, lex::DiagnosticKind::UnknownModule);
      }
      if (Child->kind == K::ast_alias_decl) {
        TypeDeclarationInfo Info;
        Info.Node = Child.get();
        Info.Module = Name;
        Info.QualifiedName =
            Name.empty() ? Child->text : Name + "." + Child->text;
        Info.Public = IsPublic(*Child);
        if ((Name.empty() && ParseBuiltinType(Child->text)) ||
            Classes.contains(Info.QualifiedName) ||
            !TypeDeclarations.emplace(Info.QualifiedName, std::move(Info))
                 .second)
          Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        for (const auto &Part : Child->children)
          if (Part->kind == K::ast_annotation)
            Error(*Part, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      if (Child->kind != K::ast_class)
        continue;
      ClassInfo Info;
      Info.Node = Child.get();
      Info.Module = Name;
      Info.Name = Child->text;
      Info.QualifiedName =
          Name.empty() ? Child->text : Name + "." + Child->text;
      Info.Public = IsPublic(*Child);
      Info.IsInterface = std::any_of(
          Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation &&
                   IsBuiltinAnnotation(Part->text, "interface");
          });
      Info.Singleton = std::any_of(
          Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation &&
                   IsBuiltinAnnotation(Part->text, "singleton");
          });
      Info.Final = std::any_of(
          Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation &&
                   IsBuiltinAnnotation(Part->text, "final");
          });
      Info.Final = Info.Final || Info.Singleton;
      if (Info.Final && Info.IsInterface)
        Error(*Child, lex::DiagnosticKind::InvalidClass);
      for (const auto &Part : Child->children) {
        if (Part->kind != K::ast_annotation ||
            !IsBuiltinAnnotation(Part->text, "layout"))
          continue;
        if (Info.IsInterface) {
          Error(*Part, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        if (Info.CLayout || Part->children.size() != 1 ||
            !Part->children.front()->text.empty() ||
            Part->children.front()->children.size() != 1 ||
            Part->children.front()->children.front()->kind != K::ast_name ||
            Part->children.front()->children.front()->text != "c") {
          Error(*Part, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        Info.CLayout = true;
      }
      const auto Key = Info.QualifiedName;
      if (ParseBuiltinType(Info.Name) || TypeDeclarations.contains(Key) ||
          !Classes.emplace(Key, std::move(Info)).second) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterMetaDeclaration(*Child, MetaKind::Class, Name, IsPublic(*Child));
    }
  }

  for (auto &[Name, Declaration] : TypeDeclarations)
    ResolveTypeDeclaration(Declaration);
  if (!Diagnostics.empty())
    return false;

  std::function<bool(const ClassInfo &, std::string_view)> InterfaceHasMethod =
      [&](const ClassInfo &Interface, std::string_view Method) {
        if (Functions.contains(Interface.QualifiedName + "." +
                               std::string(Method)))
          return true;
        for (const auto &Parent : Interface.Interfaces)
          if (InterfaceHasMethod(*GetClass(Parent), Method))
            return true;
        return false;
      };

  for (auto &[Name, Class] : Classes) {
    CurrentModule = Class.Module;
    bool SawConcrete = false;
    for (const auto &Part : Class.Node->children) {
      if (Part->kind != K::ast_base_type || Part->children.size() != 1)
        continue;
      auto Type = CheckType(*Part->children.front());
      if (!Type || !Type->IsClass()) {
        Error(*Part, lex::DiagnosticKind::InvalidClass);
        continue;
      }
      const auto *Parent = GetClass(*Type);
      if (!Parent || Parent->QualifiedName == Name) {
        Error(*Part, lex::DiagnosticKind::InvalidClass);
        continue;
      }
      if (Parent->IsInterface) {
        if (std::find(Class.Interfaces.begin(), Class.Interfaces.end(),
                      Parent->QualifiedName) != Class.Interfaces.end())
          Error(*Part, lex::DiagnosticKind::InvalidClass);
        else
          Class.Interfaces.push_back(Parent->QualifiedName);
      } else if (Class.IsInterface || SawConcrete || Parent->Final ||
                 !Class.Interfaces.empty() || Class.CLayout ||
                 Parent->CLayout) {
        Error(*Part, lex::DiagnosticKind::InvalidClass);
      } else {
        SawConcrete = true;
        Class.BaseName = Parent->QualifiedName;
        Class.Fields.push_back({Class.Node, "$base", *Type, false});
        Class.OwnFieldStart = 1;
      }
    }
  }

  std::unordered_map<std::string, unsigned> InheritanceStates;
  std::function<void(const ClassInfo &)> CheckInheritanceGraph =
      [&](const ClassInfo &Class) {
        auto &State = InheritanceStates[Class.QualifiedName];
        if (State == 2)
          return;
        if (State == 1) {
          Error(*Class.Node, lex::DiagnosticKind::RecursiveClass);
          return;
        }
        State = 1;
        if (!Class.BaseName.empty())
          CheckInheritanceGraph(*GetClass(Class.BaseName));
        for (const auto &Interface : Class.Interfaces)
          CheckInheritanceGraph(*GetClass(Interface));
        State = 2;
      };
  for (const auto &[Name, Class] : Classes)
    CheckInheritanceGraph(Class);
  if (!Diagnostics.empty())
    return false;

  for (const auto &External : ExternalFunctions) {
    FunctionInfo Info;
    Info.Module = "c";
    Info.Symbol = External.Name;
    Info.Parameters = External.Parameters;
    Info.Return = External.Return;
    Info.Public = true;
    Info.Variadic = External.Variadic;
    Info.External = &External;
    if (!Functions.emplace("c." + External.Name, std::move(Info)).second &&
        !Modules.empty())
      Diagnostics.push_back(
          {lex::DiagnosticKind::DuplicateFunction, Modules.front().Ast->Loc});
  }

  for (const auto &Input : Modules) {
    const auto &Module = *Input.Ast;
    const auto Name = ModuleName(Module);
    CurrentModule = Name;
    const auto RegisterFunction = [&](const lex::Node &Function,
                                      std::string_view Owner) {
      for (const auto &Part : Function.children)
        if (Part->kind == K::ast_parameter_pack ||
            Part->kind == K::ast_generic_pack)
          Error(*Part, lex::DiagnosticKind::UnsupportedType);
      FunctionInfo Info;
      Info.Node = &Function;
      Info.Module = Name;
      Info.Public = IsPublic(Function);
      Info.OwnerClass = Owner;
      Info.Static =
          std::any_of(Function.children.begin(), Function.children.end(),
                      [](const auto &Part) {
                        return Part->kind == K::ast_annotation &&
                               IsBuiltinAnnotation(Part->text, "static");
                      });
      if (Info.Static && (Owner.empty() || Function.kind != K::ast_function ||
                          Function.text == "copy" || Function.text == "move" ||
                          Classes.at(std::string(Owner)).IsInterface))
        Error(Function, lex::DiagnosticKind::InvalidClass);
      Info.Virtual =
          std::any_of(Function.children.begin(), Function.children.end(),
                      [](const auto &Part) {
                        return Part->kind == K::ast_annotation &&
                               IsBuiltinAnnotation(Part->text, "virtual");
                      });
      Info.Override =
          std::any_of(Function.children.begin(), Function.children.end(),
                      [](const auto &Part) {
                        return Part->kind == K::ast_annotation &&
                               IsBuiltinAnnotation(Part->text, "override");
                      });
      const auto LocalName =
          Owner.empty() ? Function.text
                        : std::string(Owner.substr(Owner.rfind('.') + 1)) +
                              "." + Function.text;
      const bool MainAnnotation =
          std::any_of(Function.children.begin(), Function.children.end(),
                      [](const auto &Part) {
                        return Part->kind == K::ast_annotation &&
                               IsBuiltinAnnotation(Part->text, "main");
                      });
      if (Owner.empty() && MainAnnotation)
        EntrypointCandidates.push_back(&Function);
      Info.Symbol = Mangle(Name, LocalName, Owner.empty() && MainAnnotation);
      if (Owner.empty() && Input.IsEntry && !MainAnnotation &&
          Function.text == "main" && Name.empty())
        Info.Symbol = "_K0F4main";
      const lex::Node *ExternAnnotation = nullptr;
      const lex::Node *CallConvAnnotation = nullptr;
      bool HasBody = false;
      for (const auto &Part : Function.children) {
        if (Part->kind == K::ast_block)
          HasBody = true;
        if (Part->kind == K::ast_annotation &&
            IsBuiltinAnnotation(Part->text, "extern")) {
          if (ExternAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          ExternAnnotation = Part.get();
        }
        if (Part->kind == K::ast_annotation &&
            IsBuiltinAnnotation(Part->text, "callconv")) {
          if (CallConvAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          CallConvAnnotation = Part.get();
        }
      }
      if (ExternAnnotation) {
        if (!Owner.empty() || HasBody || MainAnnotation)
          Error(*ExternAnnotation,
                lex::DiagnosticKind::InvalidExternDeclaration);
        Info.Symbol = Function.text;
        if (!ExternAnnotation->children.empty()) {
          if (ExternAnnotation->children.size() != 1 ||
              ExternAnnotation->children.front()->kind !=
                  K::ast_annotation_argument ||
              !ExternAnnotation->children.front()->text.empty() ||
              ExternAnnotation->children.front()->children.size() != 1 ||
              ExternAnnotation->children.front()->children.front()->kind !=
                  K::ast_literal) {
            Error(*ExternAnnotation,
                  lex::DiagnosticKind::InvalidExternDeclaration);
          } else {
            const auto &Spelling =
                ExternAnnotation->children.front()->children.front()->text;
            if (Spelling.size() < 3 || Spelling.front() != '"' ||
                Spelling.back() != '"' ||
                Spelling.find('\\') != std::string::npos)
              Error(*ExternAnnotation,
                    lex::DiagnosticKind::InvalidExternDeclaration);
            else
              Info.Symbol = Spelling.substr(1, Spelling.size() - 2);
          }
        }
      } else if (!HasBody && (Owner.empty() ||
                              !Classes.at(std::string(Owner)).IsInterface)) {
        Error(Function, lex::DiagnosticKind::InvalidExternDeclaration);
      }
      Info.Abstract = !HasBody && !Owner.empty() &&
                      Classes.at(std::string(Owner)).IsInterface;
      if (CallConvAnnotation) {
        if (!ExternAnnotation || !Owner.empty() ||
            CallConvAnnotation->children.size() != 1 ||
            CallConvAnnotation->children.front()->kind !=
                K::ast_annotation_argument ||
            !CallConvAnnotation->children.front()->text.empty() ||
            CallConvAnnotation->children.front()->children.size() != 1 ||
            CallConvAnnotation->children.front()->children.front()->kind !=
                K::ast_literal) {
          Error(*CallConvAnnotation,
                lex::DiagnosticKind::InvalidExternDeclaration);
        } else {
          const auto &Value =
              CallConvAnnotation->children.front()->children.front()->text;
          if (Value != "\"c\"" && Value != "\"system\"")
            Error(*CallConvAnnotation,
                  lex::DiagnosticKind::InvalidExternDeclaration);
        }
      }
      if (!Owner.empty() && !Info.Static) {
        Type Receiver{BuiltinType::Class, {}};
        Receiver.ClassName = Owner;
        Receiver.AddPointer();
        Info.Parameters.push_back(std::move(Receiver));
      }
      const auto Kind =
          Owner.empty()                         ? MetaKind::Function
          : Function.kind == K::ast_constructor ? MetaKind::Constructor
          : Function.kind == K::ast_destructor  ? MetaKind::Destructor
                                                : MetaKind::Method;
      const auto Id =
          RegisterMetaDeclaration(Function, Kind, Name, Info.Public);
      if (!Owner.empty()) {
        Reflection.Records[Id].QualifiedName =
            std::string(Owner) + "." + Function.text;
        const auto Parent =
            Reflection.GetId(*Classes.at(std::string(Owner)).Node);
        if (Parent)
          Reflection.Records[*Parent].Children.push_back(Id);
      }
      const auto IsInterfacePointer = [&](const Type &Value) {
        if (!Value.IsPointer() || Value.PointerDepth != 1 ||
            Value.Element != BuiltinType::Class)
          return false;
        const auto *Class = GetClass(Value.ClassName);
        return Class && Class->IsInterface;
      };
      for (const auto &Part : Function.children) {
        if (Part->kind == K::ast_parameter && !Part->children.empty() &&
            detail::IsTypeNode(Part->children.back()->kind)) {
          if (auto Parameter = CheckType(*Part->children.back())) {
            if (Parameter->IsClass() && GetClass(*Parameter)->Singleton)
              Error(*Part, lex::DiagnosticKind::ClassValueOperation);
            Info.Parameters.push_back(*Parameter);
            Types[Part.get()] = *Parameter;
            if (ExternAnnotation && !Parameter->IsPointer() &&
                !Parameter->IsFunction() && !IsNumeric(Parameter->Element) &&
                Parameter->Element != BuiltinType::Bool &&
                Parameter->Element != BuiltinType::CBool &&
                Parameter->Element != BuiltinType::Char)
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (Parameter->IsClass() && ExternAnnotation)
              Error(*Part, lex::DiagnosticKind::ClassValueOperation);
            if (ExternAnnotation && IsInterfacePointer(*Parameter))
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (Parameter->IsVoid() || Parameter->IsResults())
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            const auto ParameterId = RegisterMetaDeclaration(
                *Part, MetaKind::Parameter, Name, false);
            Reflection.Records[ParameterId].QualifiedName =
                Reflection.Records[Id].QualifiedName + "." + Part->text;
            Reflection.Records[ParameterId].Type =
                GetOrCreateMetaType(*Parameter);
            Reflection.Records[Id].Children.push_back(ParameterId);
          }
        } else if (detail::IsTypeNode(Part->kind)) {
          if (auto Return = CheckType(*Part)) {
            if (Return->IsClass() && GetClass(*Return)->Singleton)
              Error(*Part, lex::DiagnosticKind::ClassValueOperation);
            Info.Return = *Return;
            if (ExternAnnotation && !Return->IsVoid() && !Return->IsPointer() &&
                !Return->IsFunction() && !IsNumeric(Return->Element) &&
                Return->Element != BuiltinType::Bool &&
                Return->Element != BuiltinType::CBool &&
                Return->Element != BuiltinType::Char)
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (ExternAnnotation && IsInterfacePointer(*Return))
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
          }
        }
      }
      Reflection.Records[Id].Type = GetOrCreateMetaType(Info.Return);
      Reflection.Records[Id].Symbol = Info.Symbol;
      Types[&Function] = Info.Return;
      const auto Key =
          Owner.empty()
              ? (Name.empty() ? Function.text : Name + "." + Function.text)
              : std::string(Owner) + "." + Function.text;
      if (Classes.contains(Key) || TypeDeclarations.contains(Key) ||
          !Functions.emplace(Key, Info).second)
        Error(Function, lex::DiagnosticKind::DuplicateFunction);
      Symbols[&Function] = Info.Symbol;
    };
    for (const auto &Child : Module.children) {
      if (Child->kind == K::ast_module_decl)
        continue;
      if (Child->kind == K::ast_import)
        continue;
      if (Child->kind == K::ast_alias_decl)
        continue;
      if (Child->kind == K::ast_annotation_decl) {
        RegisterMetaDeclaration(*Child, MetaKind::Annotation, Name,
                                IsPublic(*Child));
        RegisterAnnotation(*Child, Name);
        continue;
      }
      if (Child->kind == K::ast_class) {
        const auto ClassIt =
            Classes.find(Name.empty() ? Child->text : Name + "." + Child->text);
        if (ClassIt == Classes.end() || ClassIt->second.Node != Child.get())
          continue;
        auto &Class = ClassIt->second;
        std::unordered_set<std::string> MemberNames;
        for (const auto &Member : Child->children) {
          if (Member->kind == K::ast_public ||
              Member->kind == K::ast_annotation ||
              Member->kind == K::ast_base_type)
            continue;
          if (!MemberNames.insert(Member->text).second ||
              (Member->kind == K::ast_function &&
               (Member->text == "init" || Member->text == "deinit")))
            Error(*Member, lex::DiagnosticKind::InvalidClass);
          if (Member->kind == K::ast_field) {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            const auto TypeNode =
                std::find_if(Member->children.begin(), Member->children.end(),
                             [](const auto &Part) {
                               return detail::IsTypeNode(Part->kind);
                             });
            if (TypeNode == Member->children.end())
              continue;
            if (auto Value = CheckType(**TypeNode)) {
              if (Value->IsVoid() || Value->IsResults())
                Error(*Member, lex::DiagnosticKind::UnsupportedType);
              Types[Member.get()] = *Value;
              const bool Static = std::any_of(
                  Member->children.begin(), Member->children.end(),
                  [](const auto &Part) {
                    return Part->kind == K::ast_annotation &&
                           IsBuiltinAnnotation(Part->text, "static");
                  });
              if (Static) {
                auto Element = *Value;
                while (Element.IsArray())
                  Element = Element.Indexed();
                if (Element.IsClass() || Element.IsRecord() ||
                    Element.IsFunction() || !GetBitWidth(Element))
                  Error(*Member, lex::DiagnosticKind::UnsupportedType);
                Class.StaticFields.push_back(
                    {Member.get(), Member->text, *Value,
                     Mangle(Name, Class.Name + "." + Member->text + ".static",
                            false),
                     IsPublic(*Member)});
              } else {
                Class.Fields.push_back(
                    {Member.get(), Member->text, *Value, IsPublic(*Member)});
              }
              const auto Id = RegisterMetaDeclaration(*Member, MetaKind::Field,
                                                      Name, IsPublic(*Member));
              Reflection.Records[Id].QualifiedName =
                  Class.QualifiedName + "." + Member->text;
              Reflection.Records[Id].Type = GetOrCreateMetaType(*Value);
              Reflection.Records[*Reflection.GetId(*Child)].Children.push_back(
                  Id);
            }
            continue;
          }
          if (Member->kind == K::ast_const_field) {
            if (!Class.IsInterface || Member->children.size() != 2 ||
                !IsConstantExpression(*Member->children[1]))
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            if (Member->children.size() == 2) {
              auto Value = CheckType(*Member->children[0]);
              if (Value && (!IsNumeric(Value->Element) &&
                            Value->Element != BuiltinType::Bool &&
                            !(Value->IsPointer() &&
                              Value->Element == BuiltinType::CChar &&
                              Member->children[1]->kind == K::ast_literal)))
                Error(*Member, lex::DiagnosticKind::UnsupportedType);
              if (Value) {
                CheckExpression(*Member->children[1], *Value);
                Class.Constants.push_back(
                    {Member.get(), Member->children[1].get(), Member->text,
                     *Value, IsPublic(*Member)});
                Types[Member.get()] = *Value;
                const auto Id = RegisterMetaDeclaration(
                    *Member, MetaKind::Field, Name, IsPublic(*Member));
                Reflection.Records[Id].QualifiedName =
                    Class.QualifiedName + "." + Member->text;
                Reflection.Records[Id].Type = GetOrCreateMetaType(*Value);
                Reflection.Records[*Reflection.GetId(*Child)]
                    .Children.push_back(Id);
              }
            }
            continue;
          }
          if (Member->kind != K::ast_function &&
              Member->kind != K::ast_constructor &&
              Member->kind != K::ast_destructor)
            continue;
          if (Class.Singleton && Member->kind == K::ast_function &&
              Member->text == "instance")
            Error(*Member, lex::DiagnosticKind::InvalidClass);
          if (Member->kind == K::ast_constructor) {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            if (Class.Constructor)
              Error(*Member, lex::DiagnosticKind::DuplicateFunction);
            Class.Constructor = Member.get();
            if (std::any_of(Member->children.begin(), Member->children.end(),
                            [](const auto &Part) {
                              return Part->kind == K::ast_generic_pack;
                            })) {
              if (!ValidForwardConstructor(*Member))
                Error(*Member, lex::DiagnosticKind::InvalidClass);
              continue;
            }
          } else if (Member->kind == K::ast_destructor) {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            if (Class.Destructor || IsPublic(*Member) ||
                std::any_of(Member->children.begin(), Member->children.end(),
                            [](const auto &Part) {
                              return Part->kind == K::ast_parameter;
                            }))
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Destructor = Member.get();
          } else if (Member->kind == K::ast_function &&
                     Member->text == "copy") {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Copy = Member.get();
          } else if (Member->kind == K::ast_function &&
                     Member->text == "move") {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Move = Member.get();
          }
          RegisterFunction(*Member, Class.QualifiedName);
        }
        Class.UserFieldCount = Class.Fields.size();
        for (const auto &Member : Child->children) {
          if (Member->kind != K::ast_function)
            continue;
          const auto It =
              Functions.find(Class.QualifiedName + "." + Member->text);
          if (It == Functions.end() || !It->second.Virtual || It->second.Static)
            continue;
          if (Class.IsInterface || It->second.Override ||
              Member->text == "copy" || Member->text == "move") {
            Error(*Member, lex::DiagnosticKind::InvalidClass);
            continue;
          }
          Type Slot{BuiltinType::Function, {}};
          Slot.Parameters = It->second.Parameters;
          Slot.Results.push_back(It->second.Return);
          Class.VirtualSlots.emplace(Member->text, Class.Fields.size());
          Class.Fields.push_back(
              {Member.get(), "$virtual." + Member->text, Slot, false});
        }
        if (Class.IsInterface) {
          Class.DefaultConstructible = false;
          continue;
        }
        if (Class.Constructor) {
          Class.ConstructorSymbol = Symbols.contains(Class.Constructor)
                                        ? Symbols.at(Class.Constructor)
                                        : std::string();
          Class.DefaultConstructible =
              !Class.ConstructorSymbol.empty() &&
              std::none_of(Class.Constructor->children.begin(),
                           Class.Constructor->children.end(),
                           [](const auto &Part) {
                             return Part->kind == K::ast_parameter;
                           });
        } else {
          // No explicit init: synthesize a default constructor named like an
          // explicit one so a class without init still constructs.
          Class.ConstructorSymbol = Mangle(Name, Class.Name + ".init", false);
          Class.DefaultConstructible = true;
        }
        if (Class.Singleton) {
          if (!Class.DefaultConstructible)
            Error(*Child, lex::DiagnosticKind::InvalidClass);
          Class.InstanceSymbol = Mangle(Name, Class.Name + ".instance", false);
        }
        if (Class.Destructor)
          Class.DestructorSymbol = Symbols.at(Class.Destructor);
        else
          Class.DestructorSymbol = Mangle(Name, Class.Name + ".deinit", false);
        const auto ValidateTransfer = [&](const lex::Node *Method,
                                          std::string_view MethodName,
                                          std::string &Symbol) {
          if (!Method) {
            Symbol =
                Mangle(Name, Class.Name + "." + std::string(MethodName), false);
            return;
          }
          const auto &Info =
              Functions.at(Class.QualifiedName + "." + std::string(MethodName));
          Type Source{BuiltinType::Class, {}};
          Source.ClassName = Class.QualifiedName;
          Source.AddPointer();
          if (Info.Parameters.size() != 2 || Info.Parameters[1] != Source ||
              !Info.Return.IsVoid())
            Error(*Method, lex::DiagnosticKind::InvalidClass);
          Symbol = Info.Symbol;
        };
        ValidateTransfer(Class.Copy, "copy", Class.CopySymbol);
        ValidateTransfer(Class.Move, "move", Class.MoveSymbol);
        continue;
      }
      if (Child->kind != K::ast_function) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterFunction(*Child, {});
    }
  }

  const auto SameInterfaceSignature = [&](const std::string &Left,
                                          const std::string &Right) {
    const auto &A = Functions.at(Left);
    const auto &B = Functions.at(Right);
    if (A.Parameters.size() != B.Parameters.size() || A.Return != B.Return)
      return false;
    for (std::size_t I = 1; I < A.Parameters.size(); ++I)
      if (A.Parameters[I] != B.Parameters[I])
        return false;
    return true;
  };
  std::function<void(ClassInfo &)> CollectInterfaceMethods =
      [&](ClassInfo &Class) {
        if (!Class.IsInterface || !Class.InterfaceMethods.empty())
          return;
        for (const auto &ParentName : Class.Interfaces) {
          auto &Parent = Classes.at(ParentName);
          CollectInterfaceMethods(Parent);
          for (const auto &Method : Parent.InterfaceMethods) {
            const auto Dot = Method.rfind('.');
            const auto Name = Method.substr(Dot + 1);
            const auto Duplicate = std::find_if(
                Class.InterfaceMethods.begin(), Class.InterfaceMethods.end(),
                [&](const auto &Existing) {
                  return Existing.substr(Existing.rfind('.') + 1) == Name;
                });
            if (Duplicate == Class.InterfaceMethods.end())
              Class.InterfaceMethods.push_back(Method);
            else if (!SameInterfaceSignature(*Duplicate, Method))
              Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
          }
        }
        for (const auto &Member : Class.Node->children) {
          if (Member->kind != K::ast_function)
            continue;
          const auto Existing = std::find_if(
              Class.InterfaceMethods.begin(), Class.InterfaceMethods.end(),
              [&](const auto &Method) {
                return Method.substr(Method.rfind('.') + 1) == Member->text;
              });
          const auto Method = Class.QualifiedName + "." + Member->text;
          if (Existing == Class.InterfaceMethods.end())
            Class.InterfaceMethods.push_back(Method);
          else if (!SameInterfaceSignature(*Existing, Method))
            Error(*Member, lex::DiagnosticKind::InvalidClass);
        }
      };
  for (auto &[Name, Class] : Classes)
    CollectInterfaceMethods(Class);

  for (auto &[Name, Class] : Classes) {
    if (Class.IsInterface)
      continue;
    for (const auto &Member : Class.Node->children) {
      if (Member->kind != K::ast_function || Member->text == "copy" ||
          Member->text == "move")
        continue;
      const auto &Method = Functions.at(Name + "." + Member->text);
      const ClassInfo *SlotOwner = nullptr;
      const FunctionInfo *Inherited = nullptr;
      for (auto BaseName = Class.BaseName; !BaseName.empty();) {
        const auto *Base = GetClass(BaseName);
        if (!Inherited) {
          const auto It = Functions.find(BaseName + "." + Member->text);
          if (It != Functions.end())
            Inherited = &It->second;
        }
        if (Base->VirtualSlots.contains(Member->text))
          SlotOwner = Base;
        BaseName = Base->BaseName;
      }
      if (Method.Static) {
        if (Method.Virtual || Method.Override || Inherited)
          Error(*Member, lex::DiagnosticKind::InvalidClass);
      } else if (Method.Override) {
        bool InterfaceMethod = false;
        for (const auto &Interface : Class.Interfaces)
          InterfaceMethod |=
              InterfaceHasMethod(*GetClass(Interface), Member->text);
        if ((!SlotOwner && !InterfaceMethod) || Method.Virtual ||
            (SlotOwner && !Inherited) ||
            (Inherited &&
             (Method.Parameters.size() != Inherited->Parameters.size() ||
              Method.Return != Inherited->Return ||
              (Inherited->Public && !Method.Public)))) {
          Error(*Member, lex::DiagnosticKind::InvalidClass);
          continue;
        }
        bool Matching = true;
        if (Inherited)
          for (std::size_t I = 1; I < Method.Parameters.size(); ++I)
            Matching &= Method.Parameters[I] == Inherited->Parameters[I];
        if (!Matching)
          Error(*Member, lex::DiagnosticKind::TypeMismatch);
        if (SlotOwner)
          Class.OverrideSlots.emplace(
              Member->text,
              std::make_pair(SlotOwner->QualifiedName,
                             SlotOwner->VirtualSlots.at(Member->text)));
      } else if (Inherited) {
        Error(*Member, lex::DiagnosticKind::InvalidClass);
      }
    }
    if (!Class.BaseName.empty() && (Class.Copy || Class.Move))
      Error(*Class.Node, lex::DiagnosticKind::InvalidClass);

    std::function<void(const ClassInfo &)> CheckInterface =
        [&](const ClassInfo &Interface) {
          for (const auto &Member : Interface.Node->children) {
            if (Member->kind != K::ast_function)
              continue;
            const auto &Required =
                Functions.at(Interface.QualifiedName + "." + Member->text);
            const FunctionInfo *Implementation = nullptr;
            for (auto OwnerName = Class.QualifiedName; !OwnerName.empty();) {
              const auto It = Functions.find(OwnerName + "." + Member->text);
              if (It != Functions.end()) {
                Implementation = &It->second;
                break;
              }
              OwnerName = GetClass(OwnerName)->BaseName;
            }
            if (!Implementation ||
                Implementation->Parameters.size() !=
                    Required.Parameters.size() ||
                Implementation->Return != Required.Return ||
                (Required.Public && !Implementation->Public)) {
              Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
              continue;
            }
            for (std::size_t I = 1; I < Required.Parameters.size(); ++I)
              if (Implementation->Parameters[I] != Required.Parameters[I])
                Error(*Class.Node, lex::DiagnosticKind::TypeMismatch);
          }
          for (const auto &Parent : Interface.Interfaces)
            CheckInterface(*GetClass(Parent));
        };
    for (const auto &Interface : Class.Interfaces)
      CheckInterface(*GetClass(Interface));
  }

  CheckClassLayouts();
  if (!Diagnostics.empty())
    return false;

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children)
      if (Child->kind == K::ast_annotation_decl)
        CheckAnnotationDefinition(*Child);
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind == K::ast_function ||
          Child->kind == K::ast_annotation_decl || Child->kind == K::ast_class)
        CheckAnnotations(*Child);
      if (Child->kind == K::ast_class)
        for (const auto &Member : Child->children)
          if (Member->kind != K::ast_public &&
              Member->kind != K::ast_annotation) {
            CheckAnnotations(*Member);
            for (const auto &Parameter : Member->children)
              if (Parameter->kind == K::ast_parameter ||
                  Parameter->kind == K::ast_parameter_pack)
                CheckAnnotations(*Parameter);
          }
      if (Child->kind == K::ast_function)
        for (const auto &Parameter : Child->children)
          if (Parameter->kind == K::ast_parameter ||
              Parameter->kind == K::ast_parameter_pack)
            CheckAnnotations(*Parameter);
    }
  }

  for (const auto &[Node, Instances] : AnnotationInstances)
    Reflection.SetAnnotations(*Node, Instances);

  const auto HasReflectAnnotation = [this](const lex::Node &Node) {
    const auto &Annotations = GetAnnotations(Node);
    return std::any_of(Annotations.begin(), Annotations.end(),
                       [](const AnnotationInstance &Annotation) {
                         return Annotation.Name == "std.annotation.reflect";
                       });
  };
  for (const auto &Input : Modules) {
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_class)
        continue;
      const auto ClassId = Reflection.GetId(*Child);
      if (!ClassId)
        continue;
      const bool ReflectClass = HasReflectAnnotation(*Child);
      bool HasReflectedField = false;
      for (const auto &Member : Child->children) {
        if (Member->kind != K::ast_field)
          continue;
        const auto FieldId = Reflection.GetId(*Member);
        if (!FieldId)
          continue;
        auto &Field = Reflection.Records[*FieldId];
        Field.RuntimeReflected =
            (ReflectClass && Field.Public) || HasReflectAnnotation(*Member);
        HasReflectedField |= Field.RuntimeReflected;
      }
      Reflection.Records[*ClassId].RuntimeReflected =
          ReflectClass || HasReflectedField;
    }
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Input.IsExternal && !Child->GenericInstance)
        continue;
      if (Child->kind != K::ast_function)
        if (Child->kind == K::ast_class) {
          const auto &Class = Classes.at(
              CurrentModule.empty() ? Child->text
                                    : CurrentModule + "." + Child->text);
          for (const auto &Member : Child->children)
            if (Member->kind == K::ast_function ||
                Member->kind == K::ast_constructor ||
                Member->kind == K::ast_destructor) {
              if (std::any_of(Member->children.begin(), Member->children.end(),
                              [](const auto &Part) {
                                return Part->kind == K::ast_generic_pack;
                              }))
                continue;
              CheckClassMember(*Member, Class);
            }
          continue;
        } else
          continue;
      CurrentClass.clear();
      if (std::any_of(
              Child->children.begin(), Child->children.end(),
              [](const auto &Part) { return Part->kind == K::ast_block; }))
        CheckFunction(*Child);
    }
  }
  return Diagnostics.empty();
}

bool sema::Sema::CheckEntrypoint(const lex::Node &Module) {
  using K = lex::TokenKind;
  const lex::Node *Entry = nullptr;
  for (const auto *Function : EntrypointCandidates) {
    if (Entry) {
      Error(*Function, lex::DiagnosticKind::InvalidEntrypoint);
      continue;
    }
    Entry = Function;
    unsigned Parameters = 0;
    const lex::Node *ReturnTypeNode = nullptr;
    bool HasBody = false;
    for (const auto &Child : Function->children) {
      if (Child->kind == K::ast_parameter)
        ++Parameters;
      else if (detail::IsTypeNode(Child->kind))
        ReturnTypeNode = Child.get();
      else if (Child->kind == K::ast_block)
        HasBody = true;
    }
    const Type Expected{BuiltinType::I32, {}};
    if (!HasBody || Parameters != 0 || !ReturnTypeNode ||
        GetType(*ReturnTypeNode) != Expected)
      Error(*Function, lex::DiagnosticKind::InvalidEntrypoint);
  }
  if (!Entry)
    Error(Module, lex::DiagnosticKind::MissingEntrypoint);
  return Diagnostics.empty();
}
