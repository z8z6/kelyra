#include "Front/Sema/Sema.h"
#include "BuiltinAnnotations.h"
#include "BuiltinCTypes.h"
#include "BuiltinMeta.h"
#include "Front/Parser/Parser.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <sstream>

using namespace kelyra;

namespace {
std::string MetaTypeName(const sema::Type &Type) {
  if (Type.IsPointer())
    return "*" + MetaTypeName(Type.Pointee());
  if (Type.IsArray())
    return "[" + std::to_string(Type.ArrayLength()) + "]" + MetaTypeName(Type.Indexed());
  if (Type.IsSlice())
    return "[]" + std::string(Type.IsReadOnlySlice() ? "const " : "") +
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
  std::string Result = Type.Element == sema::BuiltinType::Class  ? Type.ClassName
                       : Type.Element == sema::BuiltinType::Enum ? Type.EnumName
                       : Type.IsVoid()                           ? "void"
                       : Type.CName.empty()
                           ? std::string(sema::GetBuiltinTypeInfo(Type.Element).Name)
                           : Type.CName;
  return Result;
}

} // namespace

std::vector<const sema::Sema::FunctionInfo *>
sema::Sema::FindOverloads(std::string_view QualifiedName) const {
  std::vector<const FunctionInfo *> Result;
  if (const auto Group = FunctionGroups.find(std::string(QualifiedName));
      Group != FunctionGroups.end())
    for (const auto &Key : Group->second)
      Result.push_back(&Functions.at(Key));
  return Result;
}

const sema::Sema::FunctionInfo *sema::Sema::FindFunction(const lex::Node &Declaration) const {
  const auto It = FunctionKeys.find(&Declaration);
  return It == FunctionKeys.end() ? nullptr : &Functions.at(It->second);
}

std::string sema::Sema::FormatFunctionSignature(const FunctionInfo &Function) const {
  std::string Result = Function.QualifiedName + "(";
  const auto Start = Function.OwnerClass.empty() || Function.Static ? 0u : 1u;
  for (std::size_t I = Start; I < Function.Parameters.size(); ++I) {
    if (I != Start)
      Result += ", ";
    Result += MetaTypeName(Function.Parameters[I]);
  }
  return Result + ") -> " + MetaTypeName(Function.Return);
}

void sema::Sema::Error(const lex::Node &Node, lex::DiagnosticKind Kind) {
  Diagnostics.push_back({Kind, Node.Loc});
}

void sema::Sema::Error(const lex::Node &Node, lex::DiagnosticKind Kind, std::string Detail) {
  Diagnostics.push_back({Kind, Node.Loc, std::move(Detail)});
}

std::optional<std::string> sema::Sema::EvaluateIntegerConstant(const lex::Node &Expression) {
  auto Result = EvaluateConstant(Expression);
  return Result && Result->Type == ConstValue::Kind::Integer
             ? std::optional<std::string>(Result->Text)
             : std::nullopt;
}

std::optional<sema::ConstValue> sema::Sema::EvaluateConstant(const lex::Node &Expression) {
  std::unordered_set<const lex::Node *> Visiting;
  std::function<std::optional<ConstValue>(const lex::Node &)> Run;
  Run = [&](const lex::Node &Node) -> std::optional<ConstValue> {
    ConstEvaluator Evaluator(
        [&](const lex::Node &Reference) -> std::optional<ConstValue> {
          if (Reference.kind == lex::NodeKind::ast_meta && Reference.children.size() == 1) {
            const auto Id = ResolveMetaTarget(*Reference.children.front());
            if (Id)
              return ConstValue{ConstValue::Kind::Symbol, std::to_string(*Id)};
          }
          if (const auto *Variant = GetEnumVariant(Reference))
            return ConstValue::Integer(Variant->Value);
          if (const auto *External = GetExternalConstant(Reference))
            return ConstValue::Integer(External->Integer);
          if (const auto *Constant = GetConstant(Reference)) {
            if (!Visiting.insert(Constant).second)
              return std::nullopt;
            auto Result = Run(*Constant);
            Visiting.erase(Constant);
            return Result;
          }
          return std::nullopt;
        },
        [&](const lex::Node &Callee) -> const lex::Node * {
          if (MetaModule && Callee.kind == lex::NodeKind::ast_member &&
              Callee.children.size() == 1 &&
              Callee.children.front()->kind == lex::NodeKind::ast_meta &&
              Callee.children.front()->children.size() == 1) {
            const auto Id = ResolveMetaTarget(*Callee.children.front()->children.front());
            if (!Id)
              return nullptr;
            const auto Kind = Reflection.Get(*Id).Kind;
            const bool FunctionRecord = Kind == MetaKind::Function || Kind == MetaKind::Method ||
                                        Kind == MetaKind::Constructor ||
                                        Kind == MetaKind::Destructor;
            for (const auto Name : {Kind == MetaKind::Class   ? "Class"
                                    : Kind == MetaKind::Field ? "Field"
                                    : FunctionRecord          ? "Function"
                                                              : "Symbol",
                                    "Symbol"})
              for (const auto &Candidate : MetaModule->children)
                if (Candidate->kind == lex::NodeKind::ast_class && Candidate->text == Name)
                  for (const auto &Part : Candidate->children)
                    if (Part->kind == lex::NodeKind::ast_function && Part->text == Callee.text)
                      return Part.get();
            return nullptr;
          }
          const auto Name = lex::GetQualifiedName(Callee);
          if (!Name)
            return nullptr;
          auto Found = Functions.end();
          if (!Context.CurrentModule.empty() && Name->find('.') == std::string::npos)
            Found = Functions.find(Context.CurrentModule + "." + *Name);
          if (Found == Functions.end())
            Found = Functions.find(*Name);
          if (Found == Functions.end() && Name->find('.') == std::string::npos) {
            const auto Import = Imports.find(Context.CurrentModule);
            if (Import != Imports.end())
              for (const auto &Module : Import->second) {
                const auto Candidate = Functions.find(Module + "." + *Name);
                if (Candidate == Functions.end() || !Candidate->second.Public)
                  continue;
                if (Found != Functions.end())
                  return nullptr;
                Found = Candidate;
              }
          }
          if (Found != Functions.end()) {
            const auto Group = FunctionGroups.find(Found->second.QualifiedName);
            if (Group != FunctionGroups.end() && Group->second.size() > 1)
              return nullptr;
          }
          if (Found == Functions.end() || Found->second.External || !Found->second.Node ||
              std::none_of(Found->second.Node->children.begin(),
                           Found->second.Node->children.end(),
                           [](const auto &Part) { return Part->kind == lex::NodeKind::ast_block; }))
            return nullptr;
          if (Found->second.Module != Context.CurrentModule) {
            const auto Import = Imports.find(Context.CurrentModule);
            if (!Found->second.Public || Import == Imports.end() ||
                !Import->second.contains(Found->second.Module))
              return nullptr;
          }
          return Found->second.Node;
        },
        [&](const lex::Node &Call,
            const std::vector<ConstValue> &Arguments) -> std::optional<ConstValue> {
          if (auto Result = EvaluateMetaIntrinsic(Call, Arguments))
            return Result;
          return std::nullopt;
        },
        [&](const ConstValue &Owner, std::string_view Name) -> std::optional<ConstValue> {
          if (Owner.Type != ConstValue::Kind::Symbol)
            return std::nullopt;
          MetaId Id = InvalidMetaId;
          const auto Parsed =
              std::from_chars(Owner.Text.data(), Owner.Text.data() + Owner.Text.size(), Id);
          if (Parsed.ec != std::errc{} || Parsed.ptr != Owner.Text.data() + Owner.Text.size() ||
              Id >= Reflection.GetRecords().size())
            return std::nullopt;
          if (Name == "name")
            return ConstValue::String(Reflection.Get(Id).Name);
          return std::nullopt;
        });
    return Evaluator.Evaluate(Node);
  };
  return Run(Expression);
}

bool sema::Sema::CanZeroInitialize(Type Value) const {
  while (Value.IsArray())
    Value = Value.Indexed();
  if (Value.IsPointer() || !Value.IsEnum())
    return true;
  const auto *Enum = GetEnum(Value.EnumName);
  return Enum && Enum->HasZero;
}

void sema::Sema::Warn(const lex::Node &Node, std::string Message) {
  Warnings.push_back({Node.Loc, std::move(Message)});
}

void sema::Sema::WarnIfDeprecated(const lex::Node &Use, const lex::Node *Declaration) {
  if (!Declaration)
    return;
  for (const auto &Annotation : GetAnnotations(*Declaration)) {
    if (Annotation.Name != "std.annotation.deprecated")
      continue;
    std::string Message = "use of deprecated function '" + Declaration->text + "'";
    if (!Annotation.Arguments.empty() && Annotation.Arguments.front().Value.Text.size() > 2) {
      auto Detail = Annotation.Arguments.front().Value.Text;
      if (Detail.front() == '"' && Detail.back() == '"')
        Detail = Detail.substr(1, Detail.size() - 2);
      Message += ": " + Detail;
    }
    Warn(Use, std::move(Message));
  }
}

sema::MetaId sema::Sema::RegisterMetaDeclaration(const lex::Node &Node, MetaKind Kind,
                                                 std::string_view Module, bool Public) {
  MetaDeclaration Declaration;
  Declaration.Kind = Kind;
  Declaration.Name = Node.text;
  Declaration.QualifiedName = Module.empty() ? Node.text : std::string(Module) + "." + Node.text;
  Declaration.Public = Public;
  Declaration.Loc = Node.Loc;
  const auto ModuleId = Reflection.Find(Module, MetaKind::Module);
  if (ModuleId)
    Declaration.Module = *ModuleId;
  const auto Id = Reflection.Add(&Node, std::move(Declaration));
  if (ModuleId && (Kind == MetaKind::Function || Kind == MetaKind::Class ||
                   Kind == MetaKind::Enum || Kind == MetaKind::Annotation))
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
  Declaration.TypeKind = Type.IsPointer()                      ? MetaTypeKind::Pointer
                         : Type.IsArray()                      ? MetaTypeKind::Array
                         : Type.IsSlice()                      ? MetaTypeKind::Slice
                         : Type.IsEnum()                       ? MetaTypeKind::Enum
                         : (Type.IsRecord() || Type.IsClass()) ? MetaTypeKind::Record
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
  } else if (Type.IsSlice()) {
    Declaration.Type = GetOrCreateMetaType(Type.Indexed());
  }
  return Reflection.Add(nullptr, std::move(Declaration));
}

const sema::Type *sema::Sema::FindName(std::string_view Name) const {
  for (auto Scope = Context.Scopes.rbegin(); Scope != Context.Scopes.rend(); ++Scope) {
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

const sema::EnumInfo *sema::Sema::GetEnum(std::string_view Name) const {
  const auto It = Enums.find(std::string(Name));
  return It == Enums.end() ? nullptr : &It->second;
}

const sema::ClassInfo *sema::Sema::GetClass(const Type &Value) const {
  return Value.Element == BuiltinType::Class ? GetClass(Value.ClassName) : nullptr;
}

const sema::ClassFieldInfo *sema::Sema::GetField(const lex::Node &Node) const {
  const auto It = FieldReferences.find(&Node);
  if (It == FieldReferences.end())
    return nullptr;
  const auto *Class = GetClass(It->second.first);
  return Class ? &Class->Fields.at(It->second.second) : nullptr;
}

const sema::ClassStaticFieldInfo *sema::Sema::GetStaticField(const lex::Node &Node) const {
  const auto It = StaticFieldReferences.find(&Node);
  if (It == StaticFieldReferences.end())
    return nullptr;
  const auto *Class = GetClass(It->second.first);
  return Class ? &Class->StaticFields.at(It->second.second) : nullptr;
}

std::size_t sema::Sema::GetFieldIndex(const lex::Node &Node) const {
  return FieldReferences.at(&Node).second;
}

const sema::ClassInfo *sema::Sema::GetConstructorCall(const lex::Node &Node) const {
  const auto It = ConstructorCalls.find(&Node);
  return It == ConstructorCalls.end() ? nullptr : GetClass(It->second);
}

bool sema::Sema::IsClassTemporary(const lex::Node &Node) const {
  if (ForwardTemporaries.contains(&Node))
    return true;
  if (GetConstructorCall(Node))
    return true;
  if (Node.kind == lex::NodeKind::ast_group && Node.children.size() == 1)
    return IsClassTemporary(*Node.children.front());
  const auto Type = Types.find(&Node);
  return Node.kind == lex::NodeKind::ast_call && Type != Types.end() && Type->second.IsClass();
}

std::optional<sema::Type> sema::Sema::ResolveTypeDeclaration(TypeDeclarationInfo &Declaration) {
  if (Declaration.State == 2)
    return Declaration.Resolved;
  if (Declaration.State == 1) {
    Error(*Declaration.Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  Declaration.State = 1;
  const auto PreviousModule = Context.CurrentModule;
  const auto PreviousClass = Context.CurrentClass;
  Context.CurrentModule = Declaration.Module;
  Context.CurrentClass = Declaration.OwnerClass;
  auto Result = CheckType(*Declaration.Node->children.back());
  Context.CurrentModule = PreviousModule;
  Context.CurrentClass = PreviousClass;
  Declaration.Resolved = Result;
  Declaration.State = 2;
  return Result;
}

bool sema::Sema::ContainsMetaType(const Type &Value) const {
  if (!Value.EnumName.empty()) {
    const auto Enum = Enums.find(Value.EnumName);
    if (Enum != Enums.end() &&
        (MetaModules.contains(Enum->second.Module) || MetaDeclarations.contains(Enum->second.Node)))
      return true;
  }
  if (!Value.ClassName.empty()) {
    const auto Class = Classes.find(Value.ClassName);
    if (Class != Classes.end() && (MetaModules.contains(Class->second.Module) ||
                                   MetaDeclarations.contains(Class->second.Node)))
      return true;
  }
  for (const auto &Part : Value.Parameters)
    if (ContainsMetaType(Part))
      return true;
  for (const auto &Part : Value.Results)
    if (ContainsMetaType(Part))
      return true;
  return false;
}

std::optional<sema::Type> sema::Sema::CheckType(const lex::Node &Node) {
  using K = lex::NodeKind;
  if (Node.kind == K::ast_function_type)
    return CheckFunctionType(Node);
  if (Node.kind == K::ast_result_types) {
    Type Result{BuiltinType::Results, {}};
    for (const auto &Child : Node.children) {
      auto Value = CheckType(*Child);
      if (!Value)
        return std::nullopt;
      if (Value->IsVoid() || Value->IsClass() || Value->IsRecord() || Value->IsArray() ||
          Value->IsResults() || GetBitWidth(*Value) > 128) {
        Error(*Child, lex::DiagnosticKind::UnsupportedType);
        return std::nullopt;
      }
      Result.Results.push_back(*Value);
    }
    Types[&Node] = Result;
    return Result;
  }
  if (Node.kind == K::ast_type) {
    if (Node.text.find('.') == std::string::npos)
      for (auto Scope = Context.LocalTypeScopes.rbegin(); Scope != Context.LocalTypeScopes.rend();
           ++Scope)
        if (const auto Found = Scope->find(Node.text); Found != Scope->end()) {
          Types[&Node] = Found->second;
          return Found->second;
        }
    const auto Element = ParseBuiltinType(Node.text);
    const auto External = ExternalTypes.find(Node.text);
    std::string ClassName = Node.text;
    const auto &AccessModule =
        Node.GenericArgument ? Node.GenericOriginModule : Context.CurrentModule;
    if (Element && Node.text.starts_with("__c_") && AccessModule != "c") {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    if (Node.text.find('.') == std::string::npos) {
      const auto MemberName = Context.CurrentClass + "." + Node.text;
      if (!Context.CurrentClass.empty() && TypeDeclarations.contains(MemberName)) {
        ClassName = MemberName;
      } else if (!AccessModule.empty()) {
        const auto LocalName = AccessModule + "." + Node.text;
        if (Classes.contains(LocalName) || Enums.contains(LocalName) ||
            TypeDeclarations.contains(LocalName) ||
            (!Classes.contains(ClassName) && !Enums.contains(ClassName) &&
             !TypeDeclarations.contains(ClassName)))
          ClassName = LocalName;
      }
    }
    if (!Element && External == ExternalTypes.end() && !Classes.contains(ClassName) &&
        !Enums.contains(ClassName) && !TypeDeclarations.contains(ClassName) &&
        Node.text.find('.') == std::string::npos) {
      const auto Import = Imports.find(AccessModule);
      if (Import != Imports.end())
        for (const auto &Module : Import->second) {
          const auto Candidate = Module + "." + Node.text;
          const auto ImportedClass = Classes.find(Candidate);
          const auto ImportedEnum = Enums.find(Candidate);
          const auto ImportedAlias = TypeDeclarations.find(Candidate);
          if ((ImportedClass == Classes.end() || !ImportedClass->second.Public) &&
              (ImportedEnum == Enums.end() || !ImportedEnum->second.Public) &&
              (ImportedAlias == TypeDeclarations.end() || !ImportedAlias->second.Public))
            continue;
          if (ClassName != Node.text && ClassName != AccessModule + "." + Node.text) {
            Error(Node, lex::DiagnosticKind::AmbiguousName);
            return std::nullopt;
          }
          ClassName = Candidate;
        }
    }
    const auto Class = Classes.find(ClassName);
    const auto Enum = Enums.find(ClassName);
    const auto Declaration = TypeDeclarations.find(ClassName);
    if (!Element && External == ExternalTypes.end() && Class == Classes.end() &&
        Enum == Enums.end() && Declaration == TypeDeclarations.end()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Type Result;
    if (Element)
      Result = Type{*Element, {}};
    else if (External != ExternalTypes.end())
      Result = External->second;
    else if (Declaration != TypeDeclarations.end()) {
      if (!Declaration->second.OwnerClass.empty() && Declaration->second.Module != AccessModule) {
        const auto Owner = Classes.find(Declaration->second.OwnerClass);
        if (Owner == Classes.end() || !Owner->second.Public) {
          Error(Node, lex::DiagnosticKind::PrivateDeclaration);
          return std::nullopt;
        }
      }
      if (Declaration->second.Module != AccessModule) {
        const auto Import = Imports.find(AccessModule);
        const bool Imported =
            Declaration->second.Module == "c" ||
            (Import != Imports.end() && Import->second.contains(Declaration->second.Module));
        if (!Declaration->second.Public || !Imported) {
          Error(Node, lex::DiagnosticKind::PrivateDeclaration);
          return std::nullopt;
        }
      }
      auto Alias = ResolveTypeDeclaration(Declaration->second);
      if (!Alias)
        return std::nullopt;
      Result = *Alias;
    } else if (Enum != Enums.end()) {
      if (Enum->second.Module != AccessModule) {
        const auto Import = Imports.find(AccessModule);
        if (!Enum->second.Public || Import == Imports.end() ||
            !Import->second.contains(Enum->second.Module)) {
          Error(Node, lex::DiagnosticKind::PrivateDeclaration);
          return std::nullopt;
        }
      }
      Result = Type{BuiltinType::Enum, {}};
      Result.EnumName = ClassName;
      Result.BitWidth = GetBitWidth(Enum->second.Underlying);
      Result.Alignment = GetAlignment(Enum->second.Underlying);
    } else {
      if (Class->second.Module != AccessModule) {
        const auto Import = Imports.find(AccessModule);
        if (!Class->second.Public || Import == Imports.end() ||
            !Import->second.contains(Class->second.Module)) {
          Error(Node, lex::DiagnosticKind::PrivateDeclaration);
          return std::nullopt;
        }
      }
      Result = Type{BuiltinType::Class, {}};
      Result.ClassName = ClassName;
    }
    if (MetaRestrictionsReady && !Context.CurrentMetaContext && ContainsMetaType(Result)) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
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
  if (Node.kind == K::ast_slice_type && Node.children.size() == 1) {
    auto Result = CheckType(*Node.children.front());
    if (!Result || Result->IsVoid() || Result->IsResults() || Result->IsClass()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Result->AddSlice(Node.text == "const");
    Types[&Node] = *Result;
    return Result;
  }
  if (Node.kind != K::ast_array_type || Node.children.size() != 2) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  auto Result = CheckType(*Node.children.back());
  const bool PreviousMetaContext = Context.CurrentMetaContext;
  Context.CurrentMetaContext = true;
  const auto LengthType = CheckExpression(*Node.children.front(), Type{BuiltinType::USize, {}});
  Context.CurrentMetaContext = PreviousMetaContext;
  const auto Constant = EvaluateConstant(*Node.children.front());
  const std::string_view LengthText = Constant && Constant->Type == ConstValue::Kind::Integer
                                          ? std::string_view(Constant->Text)
                                          : std::string_view("0");
  std::uint64_t Length = 0;
  const auto Parsed =
      std::from_chars(LengthText.data(), LengthText.data() + LengthText.size(), Length);
  if (!Result || !LengthType || !IsInteger(LengthType->Element) || Result->IsVoid() ||
      Result->IsResults() || Parsed.ec != std::errc() || Length == 0) {
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
  Context.Scopes.emplace_back();
  Context.LocalTypeScopes.emplace_back();
  for (const auto &Statement : Block.children)
    CheckStatement(*Statement, LoopDepth);
  Context.LocalTypeScopes.pop_back();
  Context.Scopes.pop_back();
}

bool sema::Sema::AlwaysReturns(const lex::Node &Node) const {
  using K = lex::NodeKind;
  if (Node.kind == K::ast_return)
    return true;
  if (Node.kind == K::ast_block || Node.kind == K::ast_block_expr) {
    for (const auto &Child : Node.children)
      if (AlwaysReturns(*Child))
        return true;
    return false;
  }
  if (Node.kind == K::ast_when) {
    const auto *Branch = GetWhenBranch(Node);
    return Branch && AlwaysReturns(*Branch);
  }
  if (Node.kind == K::ast_expr_stmt && Node.children.size() == 1)
    return AlwaysReturns(*Node.children.front());
  if (Node.kind == K::ast_match && Node.children.size() > 1)
    return std::all_of(Node.children.begin() + 1, Node.children.end(), [this](const auto &Arm) {
      return Arm->children.size() == 2 && AlwaysReturns(*Arm->children.back());
    });
  return Node.kind == K::ast_if && Node.children.size() == 3 && AlwaysReturns(*Node.children[1]) &&
         AlwaysReturns(*Node.children[2]);
}

void sema::Sema::CheckFunction(const lex::Node &Function) {
  using K = lex::NodeKind;
  Context.Scopes.clear();
  Context.CurrentLoopDepth = 0;
  Context.Scopes.emplace_back();
  const lex::Node *ReturnTypeNode = nullptr;
  const lex::Node *Body = nullptr;
  for (const auto &Child : Function.children) {
    if (Child->kind == K::ast_parameter) {
      if (Child->children.empty() || !detail::IsTypeNode(Child->children.back()->kind))
        continue;
      auto ParameterType = CheckType(*Child->children.back());
      if (!ParameterType)
        continue;
      Types[Child.get()] = *ParameterType;
      if (!Context.Scopes.back().emplace(Child->text, *ParameterType).second)
        Error(*Child, lex::DiagnosticKind::DuplicateParameter);
    } else if (detail::IsTypeNode(Child->kind)) {
      ReturnTypeNode = Child.get();
    } else if (Child->kind == K::ast_block) {
      Body = Child.get();
    }
  }
  Context.ReturnType = ReturnTypeNode ? CheckType(*ReturnTypeNode) : std::nullopt;
  if (Context.ReturnType && Context.ReturnType->IsVoid())
    Context.ReturnType.reset();
  if (!Body) {
    Error(Function, lex::DiagnosticKind::MissingReturn);
    return;
  }
  CheckBlock(*Body);
  if (Context.ReturnType && !AlwaysReturns(*Body))
    Error(*Body, lex::DiagnosticKind::MissingReturn);
}

bool sema::Sema::Check(const lex::Node &Module) { return CheckModules({{&Module, true}}); }

bool sema::Sema::IsPublic(const lex::Node &Node) const {
  using K = lex::NodeKind;
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
  const ModuleInput *Entry = nullptr;
  for (const auto &Input : Modules)
    if (Input.IsEntry) {
      Entry = &Input;
      break;
    }
  if (!Entry && !Modules.empty())
    Entry = &Modules.front();
  std::uint64_t Hash = 1469598103934665603ull;
  const auto Feed = [&Hash](std::string_view Text) {
    for (const char Character : Text) {
      Hash ^= static_cast<unsigned char>(Character);
      Hash *= 1099511628211ull;
    }
  };
  if (Entry) {
    Feed(Entry->Ast->Loc.File);
    Feed(Entry->Name);
  }
  SymbolPrefix = "u" + std::to_string(Hash);
}

bool sema::Sema::CheckModules(const std::vector<ModuleInput> &InputModules,
                              const CDeclarations &Declarations) {
  Reset(Declarations);
  if (!Layout.IsValid())
    return false;
  auto Modules = InputModules;
  if (!PrepareModules(Modules))
    return false;
  GenerateSymbolPrefix(Modules);
  if (!RegisterTypes(Modules) || !RegisterFunctions(Modules) || !AnalyzeAnnotations(Modules) ||
      !CheckClassContracts())
    return false;
  BuildRuntimeReflection(Modules);
  return CheckBodies(Modules);
}

void sema::Sema::Reset(const CDeclarations &Declarations) {
  Context = {};
  Diagnostics.clear();
  Warnings.clear();
  Reflection.Clear();
  Types.clear();
  Functions.clear();
  FunctionGroups.clear();
  FunctionKeys.clear();
  Classes.clear();
  Enums.clear();
  TypeDeclarations.clear();
  AnnotationDeclarations.clear();
  MetaModules.clear();
  MetaDeclarations.clear();
  MetaRestrictionsReady = false;
  RuntimeDependencies.clear();
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
  ConstantReferences.clear();
  EvaluatedConstants.clear();
  MethodCalls.clear();
  FunctionValues.clear();
  IndirectCalls.clear();
  WhenBranches.clear();
  ForLoops.clear();
  CWrappers.clear();
  Imports.clear();
  ExternalTypes.clear();
  ExternalFields.clear();
  ExternalConstants.clear();
  ExternalFieldReferences.clear();
  ExternalConstantReferences.clear();
  EnumVariantReferences.clear();
  MatchPatternValues.clear();
  ExternalFunctions = Declarations.Functions;
  CHeaderSources = Declarations.Headers;
  for (const auto &External : Declarations.Types) {
    ExternalTypes.emplace(External.Name, External.Value);
    ExternalFields.emplace(External.Name, External.Fields);
  }
  for (const auto &External : Declarations.Constants)
    ExternalConstants.emplace(External.Name, External);
}

bool sema::Sema::CheckEntrypoint(const lex::Node &Module) {
  using K = lex::NodeKind;
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
    if (!HasBody || Parameters != 0 || !ReturnTypeNode || GetType(*ReturnTypeNode) != Expected)
      Error(*Function, lex::DiagnosticKind::InvalidEntrypoint);
  }
  if (!Entry)
    Error(Module, lex::DiagnosticKind::MissingEntrypoint);
  return Diagnostics.empty();
}

std::vector<std::string> sema::Sema::GetCWrapperSources() const {
  std::map<std::string, std::string> Sources;
  for (const auto &Wrapper : CWrappers)
    Sources[Wrapper.Header] += Wrapper.Source;
  std::vector<std::string> Result;
  for (auto &[Header, Source] : Sources)
    Result.push_back(std::move(Source));
  return Result;
}

std::string sema::Sema::EncodeOverloadParameters(const std::vector<sema::Type> &Parameters,
                                                 std::size_t Start) {
  constexpr char Hex[] = "0123456789ABCDEF";
  std::string Result = std::to_string(Parameters.size() - Start) + "_";
  for (std::size_t I = Start; I < Parameters.size(); ++I) {
    const auto Name = MetaTypeName(Parameters[I]);
    Result += std::to_string(Name.size()) + "_";
    for (unsigned char Byte : Name) {
      Result.push_back(Hex[Byte >> 4]);
      Result.push_back(Hex[Byte & 15]);
    }
  }
  return Result;
}

bool sema::Sema::PrepareModules(std::vector<ModuleInput> &Modules) {
  BuiltinMeta.reset();
  MetaModule = nullptr;
  for (const auto &Input : Modules)
    if (Input.Name == "std.meta") {
      MetaModule = Input.Ast;
      break;
    }
  if (!MetaModule) {
    BuiltinMeta = lex::Parser().parse(std::string(BuiltinMetaSource), "std/meta/meta.kly");
    if (!BuiltinMeta->ok())
      return false;
    MetaModule = BuiltinMeta->root.get();
  }
  const bool HasBuiltinModule = std::any_of(Modules.begin(), Modules.end(), [](const auto &Input) {
    return Input.Name == "std.annotation";
  });
  BuiltinAnnotations.reset();
  if (!HasBuiltinModule) {
    BuiltinAnnotations =
        lex::Parser().parse(std::string(BuiltinAnnotationsSource), "std/annotation/annotation.kly");
    if (!BuiltinAnnotations->ok())
      return false;
    Modules.push_back({BuiltinAnnotations->root.get(), false, true});
  }
  const bool HasCModule = std::any_of(
      Modules.begin(), Modules.end(), [](const auto &Input) { return Input.Name == "c"; });
  BuiltinCTypes.reset();
  if (!HasCModule) {
    BuiltinCTypes = lex::Parser().parse(std::string(BuiltinCTypesSource), "std/c/c.kly");
    if (!BuiltinCTypes->ok())
      return false;
    Modules.push_back({BuiltinCTypes->root.get(), false, true});
  }
  return true;
}
