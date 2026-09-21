#include "Sema/Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <sstream>

using namespace kelyra;

namespace {
std::string ModuleName(const lex::Node &Module) {
  for (const auto &Child : Module.children)
    if (Child->kind == lex::TokenKind::ast_module_decl)
      return Child->text;
  return {};
}

std::string Mangle(std::string_view Module, std::string_view Name,
                   bool IsEntrypoint) {
  if (Module.empty() || IsEntrypoint)
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

std::size_t sema::Sema::GetFieldIndex(const lex::Node &Node) const {
  return FieldReferences.at(&Node).second;
}

const sema::ClassInfo *
sema::Sema::GetConstructorCall(const lex::Node &Node) const {
  const auto It = ConstructorCalls.find(&Node);
  return It == ConstructorCalls.end() ? nullptr : GetClass(It->second);
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
    if (!Classes.contains(ClassName) &&
        Node.text.find('.') == std::string::npos)
      ClassName =
          CurrentModule.empty() ? Node.text : CurrentModule + "." + Node.text;
    const auto Class = Classes.find(ClassName);
    if (!Element && External == ExternalTypes.end() && Class == Classes.end()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Type Result;
    if (Element)
      Result = Type{*Element, {}};
    else if (External != ExternalTypes.end())
      Result = External->second;
    else {
      if (Class->second.Module != CurrentModule) {
        const auto Import = Imports.find(CurrentModule);
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
      if (Child->children.size() != 1)
        continue;
      auto ParameterType = CheckType(*Child->children.front());
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
    const std::vector<ModuleInput> &Modules,
    const std::vector<ExternalFunction> &ExternalDeclarations,
    const std::vector<ExternalType> &ExternalTypeDeclarations) {
  using K = lex::TokenKind;
  Diagnostics.clear();
  Reflection.Clear();
  Types.clear();
  Functions.clear();
  Classes.clear();
  AnnotationDeclarations.clear();
  AnnotationInstances.clear();
  Symbols.clear();
  Callees.clear();
  CWrapperCalls.clear();
  ConstructorCalls.clear();
  FieldReferences.clear();
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
      if (Child->kind != K::ast_class)
        continue;
      ClassInfo Info;
      Info.Node = Child.get();
      Info.Module = Name;
      Info.Name = Child->text;
      Info.QualifiedName =
          Name.empty() ? Child->text : Name + "." + Child->text;
      Info.Public = IsPublic(*Child);
      const auto Key = Info.QualifiedName;
      if (ParseBuiltinType(Info.Name) ||
          !Classes.emplace(Key, std::move(Info)).second) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterMetaDeclaration(*Child, MetaKind::Class, Name, IsPublic(*Child));
    }
  }

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
      FunctionInfo Info;
      Info.Node = &Function;
      Info.Module = Name;
      Info.Public = IsPublic(Function);
      Info.OwnerClass = Owner;
      const auto LocalName =
          Owner.empty() ? Function.text
                        : std::string(Owner.substr(Owner.rfind('.') + 1)) +
                              "." + Function.text;
      Info.Symbol =
          Mangle(Name, LocalName,
                 Owner.empty() && Input.IsEntry && Function.text == "main");
      if (!Owner.empty()) {
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
      for (const auto &Part : Function.children) {
        if (Part->kind == K::ast_parameter && Part->children.size() == 1) {
          if (auto Parameter = CheckType(*Part->children.front())) {
            Info.Parameters.push_back(*Parameter);
            Types[Part.get()] = *Parameter;
            if (Parameter->IsClass())
              Error(*Part, lex::DiagnosticKind::ClassValueOperation);
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
            Info.Return = *Return;
            if (Return->IsClass())
              Error(*Part, lex::DiagnosticKind::ClassValueOperation);
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
      if (Classes.contains(Key) || !Functions.emplace(Key, Info).second)
        Error(Function, lex::DiagnosticKind::DuplicateFunction);
      Symbols[&Function] = Info.Symbol;
    };
    for (const auto &Child : Module.children) {
      if (Child->kind == K::ast_module_decl)
        continue;
      if (Child->kind == K::ast_import)
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
              Member->kind == K::ast_annotation)
            continue;
          if (!MemberNames.insert(Member->text).second ||
              (Member->kind == K::ast_function &&
               (Member->text == "init" || Member->text == "deinit")))
            Error(*Member, lex::DiagnosticKind::InvalidClass);
          if (Member->kind == K::ast_field) {
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
              Class.Fields.push_back(
                  {Member.get(), Member->text, *Value, IsPublic(*Member)});
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
          if (Member->kind != K::ast_function &&
              Member->kind != K::ast_constructor &&
              Member->kind != K::ast_destructor)
            continue;
          if (Member->kind == K::ast_constructor) {
            if (Class.Constructor)
              Error(*Member, lex::DiagnosticKind::DuplicateFunction);
            Class.Constructor = Member.get();
          } else if (Member->kind == K::ast_destructor) {
            if (Class.Destructor || IsPublic(*Member) ||
                std::any_of(Member->children.begin(), Member->children.end(),
                            [](const auto &Part) {
                              return Part->kind == K::ast_parameter;
                            }))
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Destructor = Member.get();
          }
          RegisterFunction(*Member, Class.QualifiedName);
        }
        if (Class.Constructor) {
          Class.ConstructorSymbol = Symbols.contains(Class.Constructor)
                                        ? Symbols.at(Class.Constructor)
                                        : std::string();
          Class.DefaultConstructible = std::none_of(
              Class.Constructor->children.begin(),
              Class.Constructor->children.end(),
              [](const auto &Part) { return Part->kind == K::ast_parameter; });
        } else {
          // No explicit init: synthesize a default constructor named like an
          // explicit one so a class without init still constructs.
          Class.ConstructorSymbol = Mangle(Name, Class.Name + ".init", false);
          Class.DefaultConstructible = true;
        }
        if (Class.Destructor)
          Class.DestructorSymbol = Symbols.at(Class.Destructor);
        else
          Class.DestructorSymbol = Mangle(Name, Class.Name + ".deinit", false);
        continue;
      }
      if (Child->kind != K::ast_function) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterFunction(*Child, {});
    }
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
              Member->kind != K::ast_annotation)
            CheckAnnotations(*Member);
    }
  }

  for (const auto &[Node, Instances] : AnnotationInstances)
    Reflection.SetAnnotations(*Node, Instances);

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_function)
        if (Child->kind == K::ast_class) {
          const auto &Class = Classes.at(
              CurrentModule.empty() ? Child->text
                                    : CurrentModule + "." + Child->text);
          for (const auto &Member : Child->children)
            if (Member->kind == K::ast_function ||
                Member->kind == K::ast_constructor ||
                Member->kind == K::ast_destructor)
              CheckClassMember(*Member, Class);
          continue;
        } else
          continue;
      CurrentClass.clear();
      CheckFunction(*Child);
    }
  }
  return Diagnostics.empty();
}

bool sema::Sema::CheckEntrypoint(const lex::Node &Module) {
  using K = lex::TokenKind;
  for (const auto &Function : Module.children) {
    if (Function->kind != K::ast_function || Function->text != "main")
      continue;
    unsigned Parameters = 0;
    const lex::Node *ReturnTypeNode = nullptr;
    for (const auto &Child : Function->children) {
      if (Child->kind == K::ast_parameter)
        ++Parameters;
      else if (detail::IsTypeNode(Child->kind))
        ReturnTypeNode = Child.get();
    }
    const Type Expected{BuiltinType::I32, {}};
    if (Parameters != 0 || !ReturnTypeNode ||
        GetType(*ReturnTypeNode) != Expected)
      Error(*Function, lex::DiagnosticKind::InvalidEntrypoint);
    return Diagnostics.empty();
  }
  Error(Module, lex::DiagnosticKind::MissingEntrypoint);
  return false;
}
