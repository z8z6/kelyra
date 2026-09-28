#include "Sema/Sema.h"
#include "BuiltinAnnotations.h"
#include "BuiltinCTypes.h"
#include "BuiltinMeta.h"
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
std::optional<std::string> IntrinsicOperation(const lex::Node &Function,
                                              const lex::Node &Annotation) {
  using K = lex::TokenKind;
  if (Annotation.children.empty())
    return Function.text;
  if (Annotation.children.size() != 1 ||
      Annotation.children.front()->kind != K::ast_annotation_argument ||
      !Annotation.children.front()->text.empty() ||
      Annotation.children.front()->children.size() != 1 ||
      Annotation.children.front()->children.front()->kind != K::ast_literal)
    return std::nullopt;
  const auto &Spelling = Annotation.children.front()->children.front()->text;
  if (Spelling.size() < 2 || Spelling.front() != '"' ||
      Spelling.back() != '"' || Spelling.find('\\') != std::string::npos)
    return std::nullopt;
  return Spelling.substr(1, Spelling.size() - 2);
}

std::size_t IntrinsicArity(std::string_view Operation) {
  if (Operation == "__has_annotation" || Operation == "meta.has_annotation")
    return 2;
  if (Operation == "__has_member" || Operation == "__has_field" ||
      Operation == "__has_function")
    return 2;
  for (const auto Name :
       {"__read_public", "__has_constructor", "__has_default_constructor",
        "__has_destructor", "__has_base_class", "__is_interface", "__is_final",
        "__is_static", "__is_method", "__is_constructor", "__is_destructor",
        "meta.read_public"})
    if (Operation == Name)
      return 1;
  return 0;
}

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

std::optional<llvm::APInt> ParseInteger(std::string_view Text) {
  const bool Negative = Text.starts_with('-');
  if (Negative)
    Text.remove_prefix(1);
  if (Text.empty())
    return std::nullopt;
  llvm::APInt Value(256, llvm::StringRef(Text), 10);
  return Negative ? -Value : Value;
}

std::optional<std::string> NodeQualifiedName(const lex::Node &Node) {
  if (Node.kind == lex::TokenKind::ast_name)
    return Node.text;
  if (Node.kind != lex::TokenKind::ast_member || Node.children.size() != 1)
    return std::nullopt;
  auto Parent = NodeQualifiedName(*Node.children.front());
  return Parent ? std::optional<std::string>(*Parent + "." + Node.text)
                : std::nullopt;
}

std::string DecimalInteger(const llvm::APInt &Value) {
  llvm::SmallString<48> Buffer;
  Value.toString(Buffer, 10, true);
  return std::string(Buffer);
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
  std::string Result =
      Type.Element == sema::BuiltinType::Class  ? Type.ClassName
      : Type.Element == sema::BuiltinType::Enum ? Type.EnumName
      : Type.IsVoid()                           ? "void"
      : Type.CName.empty()
          ? std::string(sema::GetBuiltinTypeInfo(Type.Element).Name)
          : Type.CName;
  return Result;
}

std::string EncodeOverloadParameters(const std::vector<sema::Type> &Parameters,
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

const sema::Sema::FunctionInfo *
sema::Sema::FindFunction(const lex::Node &Declaration) const {
  const auto It = FunctionKeys.find(&Declaration);
  return It == FunctionKeys.end() ? nullptr : &Functions.at(It->second);
}

std::string sema::Sema::FormatFunctionSignature(
    const FunctionInfo &Function) const {
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

void sema::Sema::Error(const lex::Node &Node, lex::DiagnosticKind Kind,
                       std::string Detail) {
  Diagnostics.push_back({Kind, Node.Loc, std::move(Detail)});
}

std::optional<std::string>
sema::Sema::EvaluateIntegerConstant(const lex::Node &Expression) {
  auto Result = EvaluateConstant(Expression);
  return Result && Result->Type == ConstValue::Kind::Integer
             ? std::optional<std::string>(Result->Text)
             : std::nullopt;
}

std::optional<sema::ConstValue>
sema::Sema::EvaluateConstant(const lex::Node &Expression) {
  std::unordered_set<const lex::Node *> Visiting;
  std::function<std::optional<ConstValue>(const lex::Node &)> Run;
  Run = [&](const lex::Node &Node) -> std::optional<ConstValue> {
    ConstEvaluator Evaluator(
        [&](const lex::Node &Reference) -> std::optional<ConstValue> {
          if (Reference.kind == lex::TokenKind::ast_meta &&
              Reference.children.size() == 1) {
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
          if (MetaModule && Callee.kind == lex::TokenKind::ast_member &&
              Callee.children.size() == 1 &&
              Callee.children.front()->kind == lex::TokenKind::ast_meta &&
              Callee.children.front()->children.size() == 1) {
            const auto Id =
                ResolveMetaTarget(*Callee.children.front()->children.front());
            if (!Id)
              return nullptr;
            const auto Kind = Reflection.Get(*Id).Kind;
            const bool FunctionRecord =
                Kind == MetaKind::Function || Kind == MetaKind::Method ||
                Kind == MetaKind::Constructor || Kind == MetaKind::Destructor;
            for (const auto Name : {Kind == MetaKind::Class   ? "Class"
                                    : Kind == MetaKind::Field ? "Field"
                                    : FunctionRecord          ? "Function"
                                                              : "Symbol",
                                    "Symbol"})
              for (const auto &Candidate : MetaModule->children)
                if (Candidate->kind == lex::TokenKind::ast_class &&
                    Candidate->text == Name)
                  for (const auto &Part : Candidate->children)
                    if (Part->kind == lex::TokenKind::ast_function &&
                        Part->text == Callee.text)
                      return Part.get();
            return nullptr;
          }
          const auto Name = NodeQualifiedName(Callee);
          if (!Name)
            return nullptr;
          auto Found = Functions.end();
          if (!CurrentModule.empty() && Name->find('.') == std::string::npos)
            Found = Functions.find(CurrentModule + "." + *Name);
          if (Found == Functions.end())
            Found = Functions.find(*Name);
          if (Found == Functions.end() && Name->find('.') == std::string::npos) {
            const auto Import = Imports.find(CurrentModule);
            if (Import != Imports.end())
              for (const auto &Module : Import->second) {
                const auto Candidate = Functions.find(Module + "." + *Name);
                if (Candidate == Functions.end() ||
                    !Candidate->second.Public)
                  continue;
                if (Found != Functions.end())
                  return nullptr;
                Found = Candidate;
              }
          }
          if (Found != Functions.end()) {
            const auto Group =
                FunctionGroups.find(Found->second.QualifiedName);
            if (Group != FunctionGroups.end() && Group->second.size() > 1)
              return nullptr;
          }
          if (Found == Functions.end() || Found->second.External ||
              !Found->second.Node ||
              std::none_of(Found->second.Node->children.begin(),
                           Found->second.Node->children.end(),
                           [](const auto &Part) {
                             return Part->kind == lex::TokenKind::ast_block;
                           }))
            return nullptr;
          if (Found->second.Module != CurrentModule) {
            const auto Import = Imports.find(CurrentModule);
            if (!Found->second.Public || Import == Imports.end() ||
                !Import->second.contains(Found->second.Module))
              return nullptr;
          }
          return Found->second.Node;
        },
        [&](const lex::Node &Call, const std::vector<ConstValue> &Arguments)
            -> std::optional<ConstValue> {
          if (auto Result = EvaluateMetaIntrinsic(Call, Arguments))
            return Result;
          return std::nullopt;
        },
        [&](const ConstValue &Owner,
            std::string_view Name) -> std::optional<ConstValue> {
          if (Owner.Type != ConstValue::Kind::Symbol)
            return std::nullopt;
          MetaId Id = InvalidMetaId;
          const auto Parsed = std::from_chars(
              Owner.Text.data(), Owner.Text.data() + Owner.Text.size(), Id);
          if (Parsed.ec != std::errc{} ||
              Parsed.ptr != Owner.Text.data() + Owner.Text.size() ||
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
  Declaration.TypeKind = Type.IsPointer() ? MetaTypeKind::Pointer
                         : Type.IsArray() ? MetaTypeKind::Array
                         : Type.IsSlice() ? MetaTypeKind::Slice
                         : Type.IsEnum()  ? MetaTypeKind::Enum
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
  } else if (Type.IsSlice()) {
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

const sema::EnumInfo *sema::Sema::GetEnum(std::string_view Name) const {
  const auto It = Enums.find(std::string(Name));
  return It == Enums.end() ? nullptr : &It->second;
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
  const auto PreviousClass = CurrentClass;
  CurrentModule = Declaration.Module;
  CurrentClass = Declaration.OwnerClass;
  auto Result = CheckType(*Declaration.Node->children.back());
  CurrentModule = PreviousModule;
  CurrentClass = PreviousClass;
  Declaration.Resolved = Result;
  Declaration.State = 2;
  return Result;
}

bool sema::Sema::ContainsMetaType(const Type &Value) const {
  if (!Value.EnumName.empty()) {
    const auto Enum = Enums.find(Value.EnumName);
    if (Enum != Enums.end() && (MetaModules.contains(Enum->second.Module) ||
                                MetaDeclarations.contains(Enum->second.Node)))
      return true;
  }
  if (!Value.ClassName.empty()) {
    const auto Class = Classes.find(Value.ClassName);
    if (Class != Classes.end() &&
        (MetaModules.contains(Class->second.Module) ||
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
    if (Node.text.find('.') == std::string::npos)
      for (auto Scope = LocalTypeScopes.rbegin();
           Scope != LocalTypeScopes.rend(); ++Scope)
        if (const auto Found = Scope->find(Node.text); Found != Scope->end()) {
          Types[&Node] = Found->second;
          return Found->second;
        }
    const auto Element = ParseBuiltinType(Node.text);
    const auto External = ExternalTypes.find(Node.text);
    std::string ClassName = Node.text;
    const auto &AccessModule =
        Node.GenericArgument ? Node.GenericOriginModule : CurrentModule;
    if (Element && Node.text.starts_with("__c_") && AccessModule != "c") {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    if (Node.text.find('.') == std::string::npos) {
      const auto MemberName = CurrentClass + "." + Node.text;
      if (!CurrentClass.empty() && TypeDeclarations.contains(MemberName)) {
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
    if (!Element && External == ExternalTypes.end() &&
        !Classes.contains(ClassName) && !Enums.contains(ClassName) &&
        !TypeDeclarations.contains(ClassName) &&
        Node.text.find('.') == std::string::npos) {
      const auto Import = Imports.find(AccessModule);
      if (Import != Imports.end())
        for (const auto &Module : Import->second) {
          const auto Candidate = Module + "." + Node.text;
          const auto ImportedClass = Classes.find(Candidate);
          const auto ImportedEnum = Enums.find(Candidate);
          const auto ImportedAlias = TypeDeclarations.find(Candidate);
          if ((ImportedClass == Classes.end() ||
               !ImportedClass->second.Public) &&
              (ImportedEnum == Enums.end() || !ImportedEnum->second.Public) &&
              (ImportedAlias == TypeDeclarations.end() ||
               !ImportedAlias->second.Public))
            continue;
          if (ClassName != Node.text &&
              ClassName != AccessModule + "." + Node.text) {
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
      if (!Declaration->second.OwnerClass.empty() &&
          Declaration->second.Module != AccessModule) {
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
            (Import != Imports.end() &&
             Import->second.contains(Declaration->second.Module));
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
      Result.Alignment = std::max(1u, Result.BitWidth / 8);
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
    if (MetaRestrictionsReady && !CurrentMetaContext &&
        ContainsMetaType(Result)) {
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
    if (!Result || Result->IsVoid() || Result->IsResults() ||
        Result->IsClass()) {
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
  const bool PreviousMetaContext = CurrentMetaContext;
  CurrentMetaContext = true;
  const auto LengthType =
      CheckExpression(*Node.children.front(), Type{BuiltinType::USize, {}});
  CurrentMetaContext = PreviousMetaContext;
  const auto Constant = EvaluateConstant(*Node.children.front());
  const std::string_view LengthText =
      Constant && Constant->Type == ConstValue::Kind::Integer
          ? std::string_view(Constant->Text)
          : std::string_view("0");
  std::uint64_t Length = 0;
  const auto Parsed = std::from_chars(LengthText.data(),
                                      LengthText.data() + LengthText.size(),
                                      Length);
  if (!Result || !LengthType || !IsInteger(LengthType->Element) ||
      Result->IsVoid() || Result->IsResults() ||
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
  LocalTypeScopes.emplace_back();
  for (const auto &Statement : Block.children)
    CheckStatement(*Statement, LoopDepth);
  LocalTypeScopes.pop_back();
  Scopes.pop_back();
}

bool sema::Sema::AlwaysReturns(const lex::Node &Node) const {
  using K = lex::TokenKind;
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
    return std::all_of(Node.children.begin() + 1, Node.children.end(),
                       [this](const auto &Arm) {
                         return Arm->children.size() == 2 &&
                                AlwaysReturns(*Arm->children.back());
                       });
  return Node.kind == K::ast_if && Node.children.size() == 3 &&
         AlwaysReturns(*Node.children[1]) && AlwaysReturns(*Node.children[2]);
}

void sema::Sema::CheckFunction(const lex::Node &Function) {
  using K = lex::TokenKind;
  Scopes.clear();
  CurrentLoopDepth = 0;
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
    const std::vector<ExternalType> &ExternalTypeDeclarations,
    const std::vector<ExternalConstant> &ExternalConstantDeclarations) {
  using K = lex::TokenKind;
  std::vector<ModuleInput> Modules = InputModules;
  BuiltinMeta.reset();
  MetaModule = nullptr;
  for (const auto &Input : Modules)
    if (ModuleName(*Input.Ast) == "std.meta") {
      MetaModule = Input.Ast;
      break;
    }
  if (!MetaModule) {
    BuiltinMeta =
        lex::Lexer().parse(std::string(BuiltinMetaSource), "std/meta/meta.kly");
    if (!BuiltinMeta->ok())
      return false;
    MetaModule = BuiltinMeta->root.get();
  }
  const bool HasBuiltinModule =
      std::any_of(Modules.begin(), Modules.end(), [](const auto &Input) {
        return ModuleName(*Input.Ast) == "std.annotation";
      });
  BuiltinAnnotations.reset();
  if (!HasBuiltinModule) {
    BuiltinAnnotations = lex::Lexer().parse(
        std::string(BuiltinAnnotationsSource), "std/annotation/annotation.kly");
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
        lex::Lexer().parse(std::string(BuiltinCTypesSource), "std/c/c.kly");
    if (!BuiltinCTypes->ok())
      return false;
    Modules.push_back({BuiltinCTypes->root.get(), false, true});
  }
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
  LocalTypeScopes.clear();
  AnnotationDeclarations.clear();
  MetaModules.clear();
  MetaDeclarations.clear();
  CurrentMetaContext = false;
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
  CurrentClass.clear();
  CurrentConstructor = nullptr;
  ConstructionContext = nullptr;
  InitializingTarget = nullptr;
  InDestructor = false;
  ExternalFunctions = ExternalDeclarations;
  for (const auto &External : ExternalTypeDeclarations) {
    ExternalTypes.emplace(External.Name, External.Value);
    ExternalFields.emplace(External.Name, External.Fields);
  }
  for (const auto &External : ExternalConstantDeclarations)
    ExternalConstants.emplace(External.Name, External);
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
        const auto &Imported = Child->text;
        const bool BootstrapImport =
            Input.IsExternal && Name == "std.annotation" &&
            (Imported == "std.meta" || Imported == "std.util.string");
        if (Imported != "c" && !BootstrapImport &&
            !ModuleTable.contains(Imported))
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
            Enums.contains(Info.QualifiedName) ||
            !TypeDeclarations.emplace(Info.QualifiedName, std::move(Info))
                 .second)
          Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        for (const auto &Part : Child->children)
          if (Part->kind == K::ast_annotation)
            Error(*Part, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      if (Child->kind == K::ast_enum) {
        EnumInfo Info;
        Info.Node = Child.get();
        Info.Module = Name;
        Info.QualifiedName =
            Name.empty() ? Child->text : Name + "." + Child->text;
        Info.Public = IsPublic(*Child);
        if (ParseBuiltinType(Child->text) ||
            Classes.contains(Info.QualifiedName) ||
            TypeDeclarations.contains(Info.QualifiedName) ||
            !Enums.emplace(Info.QualifiedName, std::move(Info)).second)
          Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        else
          RegisterMetaDeclaration(*Child, MetaKind::Enum, Name,
                                  IsPublic(*Child));
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
      for (const auto &Part : Child->children)
        if (Part->kind == K::ast_annotation &&
            IsBuiltinAnnotation(Part->text, "intrinsic")) {
          const auto Operation = IntrinsicOperation(*Child, *Part);
          Info.RawStorage = Name == "std.memory" &&
                            Child->text.starts_with("Raw__G") && Operation &&
                            *Operation == "raw";
          if (!Info.RawStorage)
            Error(*Part, lex::DiagnosticKind::InvalidIntrinsicDeclaration);
        }
      Info.Final = std::any_of(
          Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation &&
                   IsBuiltinAnnotation(Part->text, "final");
          });
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
        if (Part->children.size() == 1 &&
            Part->children.front()->children.size() == 1) {
          const auto Value =
              NodeQualifiedName(*Part->children.front()->children.front());
          Info.CLayout |= Value && (*Value == "std.annotation.Layout.C" ||
                                    *Value == "Layout.C");
        }
      }
      const auto Key = Info.QualifiedName;
      if (ParseBuiltinType(Info.Name) || TypeDeclarations.contains(Key) ||
          Enums.contains(Key) ||
          !Classes.emplace(Key, std::move(Info)).second) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterMetaDeclaration(*Child, MetaKind::Class, Name, IsPublic(*Child));
    }
  }

  for (const auto &Input : Modules) {
    const auto Name = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_class)
        continue;
      const auto Owner = Name.empty() ? Child->text : Name + "." + Child->text;
      for (const auto &Member : Child->children) {
        if (Member->kind != K::ast_alias_decl)
          continue;
        if (ParseBuiltinType(Member->text))
          Error(*Member, lex::DiagnosticKind::UnsupportedDeclaration);
        for (const auto &Part : Member->children)
          if (Part->kind == K::ast_annotation)
            Error(*Part, lex::DiagnosticKind::InvalidAnnotation);
        if (std::any_of(Member->children.begin(), Member->children.end(),
                        [](const auto &Part) {
                          return Part->kind == K::ast_generic_parameter;
                        }))
          continue;
        TypeDeclarationInfo Info;
        Info.Node = Member.get();
        Info.Module = Name;
        Info.OwnerClass = Owner;
        Info.QualifiedName = Owner + "." + Member->text;
        Info.Public = IsPublic(*Member);
        if (!TypeDeclarations.emplace(Info.QualifiedName, std::move(Info))
                 .second)
          Error(*Member, lex::DiagnosticKind::UnsupportedDeclaration);
      }
    }
  }

  for (auto &[Name, Enum] : Enums) {
    CurrentModule = Enum.Module;
    const auto Backing =
        std::find_if(Enum.Node->children.begin(), Enum.Node->children.end(),
                     [](const auto &Part) {
                       return Part->kind == lex::TokenKind::ast_type;
                     });
    if (Backing != Enum.Node->children.end()) {
      auto Type = CheckType(**Backing);
      if (!Type || Type->IsPointer() || Type->IsArray() || Type->IsSlice() ||
          !IsInteger(Type->Element) || GetBitWidth(*Type) > 128) {
        Error(**Backing, lex::DiagnosticKind::UnsupportedType);
        continue;
      }
      Enum.Underlying = *Type;
    }
    llvm::APInt Next(256, 0);
    std::unordered_set<std::string> Names;
    std::unordered_set<std::string> Values;
    for (const auto &Part : Enum.Node->children) {
      if (Part->kind != lex::TokenKind::ast_enum_variant)
        continue;
      if (!Names.insert(Part->text).second) {
        Error(*Part, lex::DiagnosticKind::DuplicateEnumVariant);
        continue;
      }
      llvm::APInt Value = Next;
      if (!Part->children.empty()) {
        ConstEvaluator Evaluator(
            [&](const lex::Node &Identifier) -> std::optional<ConstValue> {
              for (const auto &Variant : Enum.Variants)
                if (Variant.Name == Identifier.text)
                  return ConstValue::Integer(Variant.Value);
              return std::nullopt;
            });
        const auto Constant = Evaluator.Evaluate(*Part->children.front());
        auto Evaluated =
            Constant && Constant->Type == ConstValue::Kind::Integer
                ? ParseInteger(Constant->Text)
                : std::nullopt;
        if (!Evaluated) {
          Error(*Part, lex::DiagnosticKind::InvalidEnumDiscriminant);
          continue;
        }
        Value = *Evaluated;
      }
      const auto Width = GetBitWidth(Enum.Underlying);
      if (IsSignedInteger(Enum.Underlying.Element)
              ? !Value.isSignedIntN(Width)
              : (Value.isNegative() || !Value.isIntN(Width))) {
        Error(*Part, lex::DiagnosticKind::InvalidEnumDiscriminant);
        continue;
      }
      const auto Decimal = DecimalInteger(Value);
      if (!Values.insert(Decimal).second) {
        Error(*Part, lex::DiagnosticKind::DuplicateEnumValue);
        continue;
      }
      Enum.HasZero |= Value.isZero();
      Enum.Variants.push_back({Part->text, Decimal, Part.get()});
      const auto VariantId = RegisterMetaDeclaration(
          *Part, MetaKind::EnumVariant, Enum.Module, Enum.Public);
      if (const auto EnumId = Reflection.GetId(*Enum.Node)) {
        Reflection.Records[VariantId].QualifiedName =
            Enum.QualifiedName + "." + Part->text;
        Reflection.Records[*EnumId].Children.push_back(VariantId);
      }
      Next = Value + 1;
    }
  }
  for (auto &[Name, Declaration] : TypeDeclarations)
    ResolveTypeDeclaration(Declaration);
  if (!Diagnostics.empty())
    return false;

  std::function<bool(const ClassInfo &, std::string_view, std::string_view)>
      InterfaceHasMethod = [&](const ClassInfo &Interface,
                               std::string_view Method,
                               std::string_view Signature) {
        for (const auto *Candidate :
             FindOverloads(Interface.QualifiedName + "." +
                           std::string(Method)))
          if (Candidate->Signature == Signature)
            return true;
        for (const auto &Parent : Interface.Interfaces)
          if (InterfaceHasMethod(*GetClass(Parent), Method, Signature))
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
      const lex::Node *IntrinsicAnnotation = nullptr;
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
            IsBuiltinAnnotation(Part->text, "intrinsic")) {
          if (IntrinsicAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          IntrinsicAnnotation = Part.get();
        }
        if (Part->kind == K::ast_annotation &&
            IsBuiltinAnnotation(Part->text, "callconv")) {
          if (CallConvAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          CallConvAnnotation = Part.get();
        }
      }
      if (IntrinsicAnnotation) {
        const auto Operation =
            IntrinsicOperation(Function, *IntrinsicAnnotation);
        const bool MemoryIntrinsic =
            Name == "std.memory" && Owner.empty() && Operation &&
            (Operation->starts_with("init_copy__G") ||
             Operation->starts_with("init_move__G") ||
             Operation->starts_with("assume_init__G") ||
             Operation->starts_with("drop_init__G"));
        if ((!MemoryIntrinsic && (Name != "std.meta" || HasBody || !Operation ||
                                  !IntrinsicArity(*Operation))) ||
            !Owner.empty() || MainAnnotation || ExternAnnotation ||
            CallConvAnnotation) {
          Error(*IntrinsicAnnotation,
                lex::DiagnosticKind::InvalidIntrinsicDeclaration);
        }
      }
      if (ExternAnnotation) {
        if (!Owner.empty() || HasBody || MainAnnotation)
          Error(*ExternAnnotation,
                lex::DiagnosticKind::InvalidExternDeclaration);
        Info.Symbol = Function.text;
        if (ExternAnnotation->children.empty() ||
            ExternAnnotation->children.size() > 2)
          Error(*ExternAnnotation,
                lex::DiagnosticKind::InvalidExternDeclaration);
        for (std::size_t Index = 0; Index < ExternAnnotation->children.size();
             ++Index) {
          const auto &Argument = *ExternAnnotation->children[Index];
          if (Argument.kind != K::ast_annotation_argument ||
              !Argument.text.empty() || Argument.children.size() != 1 ||
              Argument.children.front()->kind != K::ast_literal) {
            Error(*ExternAnnotation,
                  lex::DiagnosticKind::InvalidExternDeclaration);
            continue;
          }
          const auto &Spelling = Argument.children.front()->text;
          if (Spelling.size() < 3 || Spelling.front() != '"' ||
              Spelling.back() != '"' ||
              Spelling.find('\\') != std::string::npos) {
            Error(*ExternAnnotation,
                  lex::DiagnosticKind::InvalidExternDeclaration);
            continue;
          }
          if (Index == 0)
            Info.Symbol = Spelling.substr(1, Spelling.size() - 2);
          else
            Info.Library = Spelling.substr(1, Spelling.size() - 2);
        }
      } else if (!HasBody && !IntrinsicAnnotation &&
                 (Owner.empty() ||
                  !Classes.at(std::string(Owner)).IsInterface)) {
        Error(Function, lex::DiagnosticKind::InvalidExternDeclaration);
      }
      Info.Abstract = !HasBody && !Owner.empty() &&
                      Classes.at(std::string(Owner)).IsInterface;
      if (CallConvAnnotation) {
        if (!ExternAnnotation || !Owner.empty()) {
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
      Reflection.Records[Id].Static = Info.Static;
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
            Info.Parameters.push_back(*Parameter);
            Types[Part.get()] = *Parameter;
            if (ExternAnnotation && Parameter->IsSlice())
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
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
            Info.Return = *Return;
            if (ExternAnnotation && Return->IsSlice())
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
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
      if (IntrinsicAnnotation) {
        const auto Operation =
            IntrinsicOperation(Function, *IntrinsicAnnotation);
        const bool MemoryIntrinsic =
            Name == "std.memory" && Owner.empty() && Operation &&
            (Operation->starts_with("init_copy__G") ||
             Operation->starts_with("init_move__G") ||
             Operation->starts_with("assume_init__G") ||
             Operation->starts_with("drop_init__G"));
        const auto Arity = Operation ? IntrinsicArity(*Operation) : 0;
        const bool ValidReturn = Info.Return == Type{BuiltinType::Bool, {}};
        const bool NamedMemberQuery =
            Operation &&
            (*Operation == "__has_member" || *Operation == "__has_field" ||
             *Operation == "__has_function");
        const bool ValidParameters =
            Arity != 0 && Info.Parameters.size() == Arity &&
            Info.Parameters.front() == Type{BuiltinType::USize, {}} &&
            (Arity == 1 ||
             (NamedMemberQuery
                  ? Info.Parameters[1].IsReadOnlySlice() &&
                        Info.Parameters[1].Indexed() ==
                            Type{BuiltinType::U8, {}}
                  : Info.Parameters[1] == Type{BuiltinType::USize, {}}));
        if (!MemoryIntrinsic && (!Arity || !ValidReturn || !ValidParameters)) {
          Error(*IntrinsicAnnotation,
                lex::DiagnosticKind::InvalidIntrinsicDeclaration);
        }
      }
      Reflection.Records[Id].Type = GetOrCreateMetaType(Info.Return);
      const auto Key =
          Owner.empty()
              ? (Name.empty() ? Function.text : Name + "." + Function.text)
              : std::string(Owner) + "." + Function.text;
      Info.QualifiedName = Key;
      Info.Signature = Function.text + "#" +
                       EncodeOverloadParameters(Info.Parameters,
                                                Owner.empty() || Info.Static ? 0 : 1);
      auto &Group = FunctionGroups[Key];
      if (Classes.contains(Key) || Enums.contains(Key) ||
          TypeDeclarations.contains(Key) ||
          std::any_of(Group.begin(), Group.end(), [&](const auto &Existing) {
            return Functions.at(Existing).Signature == Info.Signature;
          }) ||
          (!Group.empty() && (Function.kind != K::ast_function ||
                              Function.text == "init" ||
                              Function.text == "deinit" ||
                              Function.text == "copy" ||
                              Function.text == "move" || MainAnnotation))) {
        Error(Function, lex::DiagnosticKind::DuplicateFunction);
        return;
      }
      const auto StorageKey =
          Group.empty() ? Key : Key + "#" + Info.Signature;
      Info.BaseSymbol = Info.Symbol;
      Info.Extern = ExternAnnotation != nullptr;
      Info.SpecialAbi = MainAnnotation || IntrinsicAnnotation ||
                        Info.Symbol == "_K0F4main" ||
                        (!Owner.empty() &&
                         (Function.text == "init" ||
                          Function.text == "deinit" ||
                          Function.text == "copy" ||
                          Function.text == "move"));
      Reflection.Records[Id].Symbol = Info.Symbol;
      Types[&Function] = Info.Return;
      Functions.emplace(StorageKey, Info);
      Group.push_back(StorageKey);
      FunctionKeys[&Function] = StorageKey;
      Symbols[&Function] = Info.Symbol;
    };
    for (const auto &Child : Module.children) {
      if (Child->kind == K::ast_module_decl)
        continue;
      if (Child->kind == K::ast_import)
        continue;
      if (Child->kind == K::ast_alias_decl || Child->kind == K::ast_enum)
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
        CurrentClass = Class.QualifiedName;
        std::unordered_map<std::string, bool> MemberNames;
        for (const auto &Member : Child->children) {
          if (Member->kind == K::ast_public ||
              Member->kind == K::ast_annotation ||
              Member->kind == K::ast_base_type)
            continue;
          const bool IsMethod = Member->kind == K::ast_function;
          const auto [Existing, NewName] =
              MemberNames.emplace(Member->text, IsMethod);
          if ((!NewName && (!IsMethod || !Existing->second)) ||
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
                if (!CanZeroInitialize(*Value))
                  Error(*Member,
                        lex::DiagnosticKind::InvalidEnumInitialization);
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
              Reflection.Records[Id].Static = Static;
              Reflection.Records[*Reflection.GetId(*Child)].Children.push_back(
                  Id);
            }
            continue;
          }
          if (Member->kind == K::ast_const_field) {
            if (!Class.IsInterface || Member->children.size() != 2 ||
                !EvaluateConstant(*Member->children[1]))
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
                Reflection.Records[Id].Static = true;
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
        auto &MetaMembers =
            Reflection.Records[*Reflection.GetId(*Child)].Children;
        std::stable_sort(MetaMembers.begin(), MetaMembers.end(),
                         [&](MetaId Left, MetaId Right) {
                           return Reflection.Get(Left).Loc.Offset <
                                  Reflection.Get(Right).Loc.Offset;
                         });
        Class.UserFieldCount = Class.Fields.size();
        for (const auto &Member : Child->children) {
          if (Member->kind != K::ast_function)
            continue;
          const auto *Info = FindFunction(*Member);
          if (!Info || !Info->Virtual || Info->Static)
            continue;
          if (Class.IsInterface || Info->Override ||
              Member->text == "copy" || Member->text == "move") {
            Error(*Member, lex::DiagnosticKind::InvalidClass);
            continue;
          }
          Type Slot{BuiltinType::Function, {}};
          Slot.Parameters = Info->Parameters;
          Slot.Results.push_back(Info->Return);
          Class.VirtualSlots.emplace(Info->Signature, Class.Fields.size());
          Class.Fields.push_back(
              {Member.get(), "$virtual." + Info->Signature, Slot, false});
        }
        if (Class.IsInterface) {
          Class.DefaultConstructible = false;
          CurrentClass.clear();
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
        CurrentClass.clear();
        continue;
      }
      if (Child->kind != K::ast_function) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterFunction(*Child, {});
    }
  }

  // Registration can leave malformed declarations without function metadata.
  // Stop before class and interface passes inspect those declarations.
  if (!Diagnostics.empty())
    return false;

  // Every ordinary function uses its parameter types in the exported symbol.
  // Adding an overload therefore preserves symbols of existing declarations.
  for (const auto &[Name, Group] : FunctionGroups) {
    for (const auto &Key : Group) {
      auto &Info = Functions.at(Key);
      if (!Info.Extern && !Info.SpecialAbi)
        Info.Symbol = Info.BaseSymbol + "__O" +
                      EncodeOverloadParameters(
                          Info.Parameters,
                          Info.OwnerClass.empty() || Info.Static ? 0 : 1);
      Symbols[Info.Node] = Info.Symbol;
      if (const auto Id = Reflection.GetId(*Info.Node))
        Reflection.Records[*Id].Symbol = Info.Symbol;
    }
  }
  std::unordered_map<std::string, const lex::Node *> ExportedSymbols;
  for (const auto &[Name, Info] : Functions) {
    if (!Info.Node)
      continue;
    // CheckEntrypoint diagnoses multiple @main declarations separately.
    if (Info.SpecialAbi && Info.Symbol == "main")
      continue;
    if (const auto [It, New] =
            ExportedSymbols.emplace(Info.Symbol, Info.Node);
        !New)
      Error(*Info.Node, lex::DiagnosticKind::DuplicateFunction);
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children)
      if (Child->kind == K::ast_annotation_decl)
        CheckAnnotationDefinition(*Child);
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_module_decl)
        continue;
      CheckAnnotations(*Child);
      for (const auto &Annotation : GetAnnotations(*Child))
        if (Annotation.Name == "std.annotation.meta")
          MetaModules.insert(CurrentModule);
    }
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
    for (const auto &Instance : Instances)
      if (Instance.Name == "std.annotation.meta")
        MetaDeclarations.insert(Node);
  MetaRestrictionsReady = true;

  for (const auto &[Name, Class] : Classes) {
    if (MetaModules.contains(Class.Module) ||
        MetaDeclarations.contains(Class.Node))
      continue;
    if (!Class.BaseName.empty()) {
      const auto Base = Classes.find(Class.BaseName);
      if (Base != Classes.end() &&
          (MetaModules.contains(Base->second.Module) ||
           MetaDeclarations.contains(Base->second.Node)))
        Error(*Class.Node, lex::DiagnosticKind::UnsupportedType);
    }
    for (const auto &Field : Class.Fields)
      if (ContainsMetaType(Field.Value))
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
    for (const auto &Field : Class.StaticFields)
      if (ContainsMetaType(Field.Value))
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
  }
  for (const auto &[Name, Function] : Functions) {
    if (Function.Node && MetaDeclarations.contains(Function.Node) &&
        (Function.Virtual || Function.Override ||
         (!Function.OwnerClass.empty() &&
          Classes.at(Function.OwnerClass).IsInterface)))
      Error(*Function.Node, lex::DiagnosticKind::InvalidAnnotationTarget);
    if (MetaModules.contains(Function.Module) || !Function.Node ||
        MetaDeclarations.contains(Function.Node) ||
        (!Function.OwnerClass.empty() &&
         MetaDeclarations.contains(Classes.at(Function.OwnerClass).Node)))
      continue;
    if (ContainsMetaType(Function.Return) ||
        std::any_of(Function.Parameters.begin(), Function.Parameters.end(),
                    [this](const Type &Parameter) {
                      return ContainsMetaType(Parameter);
                    }))
      Error(*Function.Node, lex::DiagnosticKind::UnsupportedType);
  }
  for (const auto *Entry : EntrypointCandidates)
    if (MetaDeclarations.contains(Entry) ||
        std::any_of(Functions.begin(), Functions.end(),
                    [&](const auto &Function) {
                      return Function.second.Node == Entry &&
                             MetaModules.contains(Function.second.Module);
                    }))
      Error(*Entry, lex::DiagnosticKind::InvalidEntrypoint);

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
            const auto Duplicate = std::find_if(
                Class.InterfaceMethods.begin(), Class.InterfaceMethods.end(),
                [&](const auto &Existing) {
                  return Functions.at(Existing).Signature ==
                         Functions.at(Method).Signature;
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
          const auto Method = FunctionKeys.at(Member.get());
          const auto Existing = std::find_if(
              Class.InterfaceMethods.begin(), Class.InterfaceMethods.end(),
              [&](const auto &Candidate) {
                return Functions.at(Candidate).Signature ==
                       Functions.at(Method).Signature;
              });
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
      const auto &Method = *FindFunction(*Member);
      const ClassInfo *SlotOwner = nullptr;
      const FunctionInfo *Inherited = nullptr;
      for (auto BaseName = Class.BaseName; !BaseName.empty();) {
        const auto *Base = GetClass(BaseName);
        if (!Inherited) {
          for (const auto *Candidate :
               FindOverloads(BaseName + "." + Member->text))
            if (Candidate->Signature == Method.Signature) {
              Inherited = Candidate;
              break;
            }
        }
        if (Base->VirtualSlots.contains(Method.Signature))
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
              InterfaceHasMethod(*GetClass(Interface), Member->text,
                                 Method.Signature);
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
              Method.Signature,
              std::make_pair(SlotOwner->QualifiedName,
                             SlotOwner->VirtualSlots.at(Method.Signature)));
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
            const auto &Required = *FindFunction(*Member);
            const FunctionInfo *Implementation = nullptr;
            for (auto OwnerName = Class.QualifiedName; !OwnerName.empty();) {
              for (const auto *Candidate :
                   FindOverloads(OwnerName + "." + Member->text))
                if (Candidate->Signature == Required.Signature) {
                  Implementation = Candidate;
                  break;
                }
              if (Implementation)
                break;
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
              CurrentMetaContext = MetaModules.contains(CurrentModule) ||
                                   MetaDeclarations.contains(Child.get()) ||
                                   MetaDeclarations.contains(Member.get());
              CheckClassMember(*Member, Class);
              CurrentMetaContext = false;
            }
          continue;
        } else
          continue;
      CurrentClass.clear();
      if (std::any_of(
              Child->children.begin(), Child->children.end(),
              [](const auto &Part) { return Part->kind == K::ast_block; })) {
        CurrentMetaContext = MetaModules.contains(CurrentModule) ||
                             MetaDeclarations.contains(Child.get());
        CheckFunction(*Child);
        CurrentMetaContext = false;
      }
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
