#include "Sema/Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <charconv>
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
  std::string Result =
      Type.CName.empty()
          ? std::string(sema::GetBuiltinTypeInfo(Type.Element).Name)
          : Type.CName;
  Result.insert(0, Type.PointerDepth, '*');
  for (const auto Dimension : Type.Dimensions)
    Result += "[" + std::to_string(Dimension) + "]";
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
  if (ModuleId && (Kind == MetaKind::Function || Kind == MetaKind::Struct ||
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
  Declaration.TypeKind = Type.IsPointer()  ? MetaTypeKind::Pointer
                         : Type.IsArray()  ? MetaTypeKind::Array
                         : Type.IsRecord() ? MetaTypeKind::Record
                                           : MetaTypeKind::Builtin;
  Declaration.BitWidth = GetBitWidth(Type);
  Declaration.PointerDepth = Type.PointerDepth;
  Declaration.Dimensions = Type.Dimensions;
  if (Type.IsPointer()) {
    auto Pointee = Type;
    --Pointee.PointerDepth;
    Declaration.Type = GetOrCreateMetaType(Pointee);
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

std::optional<sema::Type> sema::Sema::CheckType(const lex::Node &Node) {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_type) {
    const auto Element = ParseBuiltinType(Node.text);
    const auto External = ExternalTypes.find(Node.text);
    if (!Element && External == ExternalTypes.end()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Type Result = Element ? Type{*Element, {}} : External->second;
    Types[&Node] = Result;
    return Result;
  }
  if (Node.kind == K::ast_pointer_type && Node.children.size() == 1) {
    auto Result = CheckType(*Node.children.front());
    if (!Result || Result->IsArray()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    ++Result->PointerDepth;
    Result->BitWidth = sizeof(void *) * 8;
    Result->Alignment = alignof(void *);
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
  if (!Result || Parsed.ec != std::errc() || Length == 0) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  if (!Result->IsRecord() && GetBitWidth(*Result) > 128) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  Result->Dimensions.push_back(Length);
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
  if (!ReturnTypeNode)
    Error(Function, lex::DiagnosticKind::UnsupportedType);
  if (!Body) {
    Error(Function, lex::DiagnosticKind::MissingReturn);
    return;
  }
  CheckBlock(*Body);
  if (!AlwaysReturns(*Body))
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

bool sema::Sema::CheckModules(
    const std::vector<ModuleInput> &Modules,
    const std::vector<ExternalFunction> &ExternalDeclarations,
    const std::vector<ExternalType> &ExternalTypeDeclarations) {
  using K = lex::TokenKind;
  Diagnostics.clear();
  Reflection.Clear();
  Types.clear();
  Functions.clear();
  AnnotationDeclarations.clear();
  AnnotationInstances.clear();
  Symbols.clear();
  Callees.clear();
  CWrapperCalls.clear();
  WhenBranches.clear();
  CWrappers.clear();
  Imports.clear();
  ExternalTypes.clear();
  ExternalFunctions = ExternalDeclarations;
  for (const auto &External : ExternalTypeDeclarations)
    ExternalTypes.emplace(External.Name, External.Value);
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
    for (const auto &Child : Module.children) {
      if (Child->kind == K::ast_module_decl)
        continue;
      if (Child->kind == K::ast_import) {
        Imports[Name].insert(Child->text);
        const auto Imported =
            Child->text.ends_with(".*")
                ? Child->text.substr(0, Child->text.size() - 2)
                : Child->text;
        if (Imported != "c" && !ModuleTable.contains(Imported))
          Error(*Child, lex::DiagnosticKind::UnknownModule);
        continue;
      }
      if (Child->kind == K::ast_annotation_decl) {
        RegisterMetaDeclaration(*Child, MetaKind::Annotation, Name,
                                IsPublic(*Child));
        RegisterAnnotation(*Child, Name);
        continue;
      }
      if (Child->kind != K::ast_function) {
        if (Child->kind == K::ast_struct)
          RegisterMetaDeclaration(*Child, MetaKind::Struct, Name,
                                  IsPublic(*Child));
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      const auto FunctionMeta = RegisterMetaDeclaration(
          *Child, MetaKind::Function, Name, IsPublic(*Child));
      FunctionInfo Info;
      Info.Node = Child.get();
      Info.Module = Name;
      Info.Public = IsPublic(*Child);
      Info.Symbol =
          Mangle(Name, Child->text, Input.IsEntry && Child->text == "main");
      for (const auto &Part : Child->children) {
        if (Part->kind == K::ast_parameter && Part->children.size() == 1) {
          if (auto Parameter = CheckType(*Part->children.front())) {
            Info.Parameters.push_back(*Parameter);
            Types[Part.get()] = *Parameter;
            auto ParameterMeta = RegisterMetaDeclaration(
                *Part, MetaKind::Parameter, Name, false);
            auto &ParameterRecord = Reflection.Records[ParameterMeta];
            ParameterRecord.QualifiedName =
                Reflection.Records[FunctionMeta].QualifiedName + "." +
                Part->text;
            ParameterRecord.Type = GetOrCreateMetaType(*Parameter);
            Reflection.Records[FunctionMeta].Children.push_back(ParameterMeta);
          }
        } else if (detail::IsTypeNode(Part->kind)) {
          if (auto Return = CheckType(*Part)) {
            Info.Return = *Return;
            Reflection.Records[FunctionMeta].Type =
                GetOrCreateMetaType(*Return);
          }
        }
      }
      const auto Key = Name.empty() ? Child->text : Name + "." + Child->text;
      if (!Functions.emplace(Key, Info).second)
        Error(*Child, lex::DiagnosticKind::DuplicateFunction);
      Symbols[Child.get()] = std::move(Info.Symbol);
      Reflection.Records[FunctionMeta].Symbol = Symbols[Child.get()];
    }
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children)
      if (Child->kind == K::ast_annotation_decl)
        CheckAnnotationDefinition(*Child);
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children)
      if (Child->kind == K::ast_function ||
          Child->kind == K::ast_annotation_decl || Child->kind == K::ast_struct)
        CheckAnnotations(*Child);
  }

  for (const auto &[Node, Instances] : AnnotationInstances)
    Reflection.SetAnnotations(*Node, Instances);

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_function)
        continue;
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
