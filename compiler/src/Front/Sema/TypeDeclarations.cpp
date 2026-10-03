#include "Front/Sema/Sema.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include <algorithm>
#include <functional>
#include <sstream>
using namespace kelyra;

namespace {
std::optional<llvm::APInt> ParseInteger(std::string_view Text) {
  const bool Negative = Text.starts_with('-');
  if (Negative)
    Text.remove_prefix(1);
  if (Text.empty())
    return std::nullopt;
  llvm::APInt Value(256, llvm::StringRef(Text), 10);
  return Negative ? -Value : Value;
}

std::string DecimalInteger(const llvm::APInt &Value) {
  llvm::SmallString<48> Buffer;
  Value.toString(Buffer, 10, true);
  return std::string(Buffer);
}

} // namespace

bool sema::Sema::RegisterTypes(const std::vector<ModuleInput> &Modules) {
  using K = lex::NodeKind;
  std::unordered_map<std::string, const lex::Node *> ModuleTable;

  for (const auto &Input : Modules) {
    const auto &Module = *Input.Ast;
    if (Module.kind != K::ast_module) {
      Error(Module, lex::DiagnosticKind::UnsupportedDeclaration);
      continue;
    }
    const auto Name = Input.Name;
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
    const auto Name = Input.Name;
    Context.CurrentModule = Name;
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind == K::ast_import) {
        Imports[Name].insert(Child->text);
        const auto &Imported = Child->text;
        const bool BootstrapImport = Input.IsExternal && Name == "std.annotation" &&
                                     (Imported == "std.meta" || Imported == "std.util.string");
        if (Imported != "c" && !BootstrapImport && !ModuleTable.contains(Imported))
          Error(*Child, lex::DiagnosticKind::UnknownModule);
      }
      if (Child->kind == K::ast_alias_decl) {
        TypeDeclarationInfo Info;
        Info.Node = Child.get();
        Info.Module = Name;
        Info.QualifiedName = Name.empty() ? Child->text : Name + "." + Child->text;
        Info.Public = IsPublic(*Child);
        if ((Name.empty() && ParseBuiltinType(Child->text)) ||
            Classes.contains(Info.QualifiedName) || Enums.contains(Info.QualifiedName) ||
            !TypeDeclarations.emplace(Info.QualifiedName, std::move(Info)).second)
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
        Info.QualifiedName = Name.empty() ? Child->text : Name + "." + Child->text;
        Info.Public = IsPublic(*Child);
        if (ParseBuiltinType(Child->text) || Classes.contains(Info.QualifiedName) ||
            TypeDeclarations.contains(Info.QualifiedName) ||
            !Enums.emplace(Info.QualifiedName, std::move(Info)).second)
          Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        else
          RegisterMetaDeclaration(*Child, MetaKind::Enum, Name, IsPublic(*Child));
        continue;
      }
      if (Child->kind != K::ast_class)
        continue;
      ClassInfo Info;
      Info.Node = Child.get();
      Info.Module = Name;
      Info.Name = Child->text;
      Info.QualifiedName = Name.empty() ? Child->text : Name + "." + Child->text;
      Info.Public = IsPublic(*Child);
      Info.IsInterface =
          std::any_of(Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "interface");
          });
      for (const auto &Part : Child->children)
        if (Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "intrinsic")) {
          const auto Operation = detail::IntrinsicOperation(*Child, *Part);
          Info.RawStorage = Name == "std.memory" && Child->text.starts_with("Raw__G") &&
                            Operation && *Operation == "raw";
          if (!Info.RawStorage)
            Error(*Part, lex::DiagnosticKind::InvalidIntrinsicDeclaration);
        }
      Info.Final =
          std::any_of(Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "final");
          });
      if (Info.Final && Info.IsInterface)
        Error(*Child, lex::DiagnosticKind::InvalidClass);
      for (const auto &Part : Child->children) {
        if (Part->kind != K::ast_annotation || !IsBuiltinAnnotation(Part->text, "layout"))
          continue;
        if (Info.IsInterface) {
          Error(*Part, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        if (Part->children.size() == 1 && Part->children.front()->children.size() == 1) {
          const auto Value = lex::GetQualifiedName(*Part->children.front()->children.front());
          Info.CLayout |= Value && (*Value == "std.annotation.Layout.C" || *Value == "Layout.C");
        }
      }
      const auto Key = Info.QualifiedName;
      if (ParseBuiltinType(Info.Name) || TypeDeclarations.contains(Key) || Enums.contains(Key) ||
          !Classes.emplace(Key, std::move(Info)).second) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterMetaDeclaration(*Child, MetaKind::Class, Name, IsPublic(*Child));
    }
  }

  for (const auto &Input : Modules) {
    const auto Name = Input.Name;
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
        if (std::any_of(Member->children.begin(), Member->children.end(), [](const auto &Part) {
              return Part->kind == K::ast_generic_parameter;
            }))
          continue;
        TypeDeclarationInfo Info;
        Info.Node = Member.get();
        Info.Module = Name;
        Info.OwnerClass = Owner;
        Info.QualifiedName = Owner + "." + Member->text;
        Info.Public = IsPublic(*Member);
        if (!TypeDeclarations.emplace(Info.QualifiedName, std::move(Info)).second)
          Error(*Member, lex::DiagnosticKind::UnsupportedDeclaration);
      }
    }
  }

  for (auto &[Name, Enum] : Enums) {
    Context.CurrentModule = Enum.Module;
    const auto Backing =
        std::find_if(Enum.Node->children.begin(), Enum.Node->children.end(), [](const auto &Part) {
          return Part->kind == lex::NodeKind::ast_type;
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
      if (Part->kind != lex::NodeKind::ast_enum_variant)
        continue;
      if (!Names.insert(Part->text).second) {
        Error(*Part, lex::DiagnosticKind::DuplicateEnumVariant);
        continue;
      }
      llvm::APInt Value = Next;
      if (!Part->children.empty()) {
        ConstEvaluator Evaluator([&](const lex::Node &Identifier) -> std::optional<ConstValue> {
          for (const auto &Variant : Enum.Variants)
            if (Variant.Name == Identifier.text)
              return ConstValue::Integer(Variant.Value);
          return std::nullopt;
        });
        const auto Constant = Evaluator.Evaluate(*Part->children.front());
        auto Evaluated = Constant && Constant->Type == ConstValue::Kind::Integer
                             ? ParseInteger(Constant->Text)
                             : std::nullopt;
        if (!Evaluated) {
          Error(*Part, lex::DiagnosticKind::InvalidEnumDiscriminant);
          continue;
        }
        Value = *Evaluated;
      }
      const auto Width = GetBitWidth(Enum.Underlying);
      if (IsSignedInteger(Enum.Underlying.Element) ? !Value.isSignedIntN(Width)
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
      const auto VariantId =
          RegisterMetaDeclaration(*Part, MetaKind::EnumVariant, Enum.Module, Enum.Public);
      if (const auto EnumId = Reflection.GetId(*Enum.Node)) {
        Reflection.Records[VariantId].QualifiedName = Enum.QualifiedName + "." + Part->text;
        Reflection.Records[*EnumId].Children.push_back(VariantId);
      }
      Next = Value + 1;
    }
  }
  for (auto &[Name, Declaration] : TypeDeclarations)
    ResolveTypeDeclaration(Declaration);
  if (!Diagnostics.empty())
    return false;

  for (auto &[Name, Class] : Classes) {
    Context.CurrentModule = Class.Module;
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
        if (std::find(Class.Interfaces.begin(), Class.Interfaces.end(), Parent->QualifiedName) !=
            Class.Interfaces.end())
          Error(*Part, lex::DiagnosticKind::InvalidClass);
        else
          Class.Interfaces.push_back(Parent->QualifiedName);
      } else if (Class.IsInterface || SawConcrete || Parent->Final || !Class.Interfaces.empty() ||
                 Class.CLayout || Parent->CLayout) {
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
  std::function<void(const ClassInfo &)> CheckInheritanceGraph = [&](const ClassInfo &Class) {
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

  return Diagnostics.empty();
}
