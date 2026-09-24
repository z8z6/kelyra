#include "Sema/Sema.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"

#include <algorithm>
#include <unordered_set>

using namespace kelyra;

namespace {
std::optional<std::string> QualifiedName(const lex::Node &Node) {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_name)
    return Node.text;
  if (Node.kind != K::ast_member || Node.children.size() != 1)
    return std::nullopt;
  auto Base = QualifiedName(*Node.children.front());
  if (!Base)
    return std::nullopt;
  return *Base + "." + Node.text;
}

bool IsAnnotationType(std::string_view Name) {
  if (Name == "meta.string" || Name == "meta.symbol" || Name == "meta.type")
    return true;
  const auto Type = sema::ParseBuiltinType(Name);
  return Type && (sema::IsNumeric(*Type) || *Type == sema::BuiltinType::Bool ||
                  *Type == sema::BuiltinType::Char);
}
} // namespace

void sema::Sema::RegisterAnnotation(const lex::Node &Declaration,
                                    std::string_view Module) {
  using K = lex::TokenKind;
  if (Module != BuiltinAnnotationModule &&
      (Declaration.text == "layout" || Declaration.text == "cfg" ||
       Declaration.text == "extern" || Declaration.text == "callconv" ||
       Declaration.text == "interface" || Declaration.text == "final" ||
       Declaration.text == "virtual" || Declaration.text == "override" ||
       Declaration.text == "main" || Declaration.text == "reflect" ||
       Declaration.text == "inline" || Declaration.text == "deprecated" ||
       Declaration.text == "target" || Declaration.text == "repeatable" ||
       Declaration.text == "retention")) {
    Error(Declaration, lex::DiagnosticKind::InvalidAnnotation);
    return;
  }
  AnnotationInfo Info;
  Info.Node = &Declaration;
  Info.Module = Module;
  Info.Public = IsPublic(Declaration);
  bool SawDefault = false;
  std::unordered_set<std::string> Names;
  for (const auto &Child : Declaration.children) {
    if (Child->kind != K::ast_annotation_parameter)
      continue;
    if (Child->children.empty() || Child->children.size() > 2 ||
        Child->children.front()->kind != K::ast_type ||
        !IsAnnotationType(Child->children.front()->text)) {
      Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
    if (!Names.emplace(Child->text).second) {
      Error(*Child, lex::DiagnosticKind::DuplicateAnnotationParameter);
      continue;
    }
    const auto *Default =
        Child->children.size() == 2 ? Child->children.back().get() : nullptr;
    if (!Default && SawDefault)
      Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
    SawDefault |= Default != nullptr;
    Info.Parameters.push_back(
        {Child->text, Child->children.front()->text, Default});
  }
  const auto Key = Info.Module.empty() ? Declaration.text
                                       : Info.Module + "." + Declaration.text;
  if (!AnnotationDeclarations.emplace(Key, std::move(Info)).second) {
    Error(Declaration, lex::DiagnosticKind::DuplicateAnnotation);
    return;
  }
  const auto AnnotationMeta = Reflection.GetId(Declaration);
  if (!AnnotationMeta)
    return;
  for (const auto &Child : Declaration.children) {
    if (Child->kind != K::ast_annotation_parameter || Child->children.empty())
      continue;
    auto ParameterMeta = RegisterMetaDeclaration(
        *Child, MetaKind::AnnotationParameter, Module, false);
    auto &ParameterRecord = Reflection.Records[ParameterMeta];
    ParameterRecord.QualifiedName =
        Reflection.Records[*AnnotationMeta].QualifiedName + "." + Child->text;
    const auto &TypeName = Child->children.front()->text;
    if (const auto Element = ParseBuiltinType(TypeName)) {
      ParameterRecord.Type = GetOrCreateMetaType(Type{*Element, {}});
    } else {
      auto TypeId = Reflection.Find(TypeName, MetaKind::Type);
      if (!TypeId) {
        MetaDeclaration MetaType;
        MetaType.Kind = MetaKind::Type;
        MetaType.Name = TypeName;
        MetaType.QualifiedName = TypeName;
        MetaType.TypeKind = MetaTypeKind::Builtin;
        TypeId = Reflection.Add(nullptr, std::move(MetaType));
      }
      ParameterRecord.Type = *TypeId;
    }
    Reflection.Records[*AnnotationMeta].Children.push_back(ParameterMeta);
  }
}

std::optional<sema::AnnotationValue>
sema::Sema::ParseAnnotationValue(const lex::Node &Expression,
                                 std::string_view ExpectedType) {
  using K = lex::TokenKind;
  if (ExpectedType == "meta.string") {
    if (Expression.kind == K::ast_literal && !Expression.text.empty() &&
        Expression.text.front() == '"')
      return AnnotationValue{AnnotationValueKind::String, Expression.text};
    return std::nullopt;
  }
  if (ExpectedType == "meta.symbol" || ExpectedType == "meta.type") {
    const lex::Node *Target = &Expression;
    if (Expression.kind == K::ast_meta && Expression.children.size() == 1)
      Target = Expression.children.front().get();
    if (ExpectedType == "meta.type" &&
        (Target->kind == K::ast_type || Target->kind == K::ast_pointer_type ||
         Target->kind == K::ast_array_type)) {
      auto Resolved = CheckType(*Target);
      if (!Resolved)
        return std::nullopt;
      const auto Id = GetOrCreateMetaType(*Resolved);
      return AnnotationValue{AnnotationValueKind::Type, Reflection.Get(Id).Name,
                             Id};
    }
    std::optional<std::string> Name;
    if (Target->kind == K::ast_type)
      Name = Target->text;
    else
      Name = QualifiedName(*Target);
    if (!Name)
      return std::nullopt;
    if (ExpectedType == "meta.type") {
      std::optional<Type> Resolved;
      if (const auto Element = ParseBuiltinType(*Name))
        Resolved = Type{*Element, {}};
      else {
        lex::Node TypeNode{K::ast_type, Target->Loc, *Name, {}, 1};
        Resolved = CheckType(TypeNode);
        Types.erase(&TypeNode);
      }
      if (!Resolved)
        return std::nullopt;
      const auto Id = GetOrCreateMetaType(*Resolved);
      return AnnotationValue{AnnotationValueKind::Type, std::move(*Name), Id};
    }

    std::string Key = *Name;
    if (Name->find('.') == std::string::npos && !CurrentModule.empty())
      Key = CurrentModule + "." + *Name;
    const auto Function = Functions.find(Key);
    if (Function == Functions.end() || !Function->second.Node) {
      if (ExpectedType == "meta.symbol" &&
          (*Name == "auto" || *Name == "always"))
        return AnnotationValue{AnnotationValueKind::Symbol, std::move(*Name)};
      return std::nullopt;
    }
    if (Function->second.Module != CurrentModule) {
      const auto Import = Imports.find(CurrentModule);
      if (!Function->second.Public || Import == Imports.end() ||
          (!Import->second.contains(Function->second.Module) &&
           !Import->second.contains(Function->second.Module + ".*")))
        return std::nullopt;
    }
    const auto Id = Reflection.GetId(*Function->second.Node);
    if (!Id)
      return std::nullopt;
    return AnnotationValue{AnnotationValueKind::Symbol, std::move(*Name), *Id};
  }
  const auto Element = ParseBuiltinType(ExpectedType);
  if (!Element)
    return std::nullopt;
  if (*Element == BuiltinType::Bool) {
    if (Expression.kind == K::ast_literal &&
        (Expression.text == "true" || Expression.text == "false"))
      return AnnotationValue{AnnotationValueKind::Bool, Expression.text};
    return std::nullopt;
  }
  const lex::Node *Literal = &Expression;
  std::string Prefix;
  if (Expression.kind == K::ast_unary && Expression.children.size() == 1 &&
      (Expression.text == "+" || Expression.text == "-")) {
    Prefix = Expression.text;
    Literal = Expression.children.front().get();
  }
  if (Literal->kind != K::ast_literal || Literal->text.empty() ||
      Literal->text.front() == '"')
    return std::nullopt;
  if (IsInteger(*Element) || *Element == BuiltinType::Char) {
    if (Literal->text.find_first_of(".eE") != std::string::npos ||
        !detail::FitsInteger(Literal->text, Type{*Element, {}}, Prefix == "-"))
      return std::nullopt;
    return AnnotationValue{AnnotationValueKind::Integer,
                           Prefix + Literal->text};
  }
  if (IsFloat(*Element))
    return AnnotationValue{AnnotationValueKind::Float, Prefix + Literal->text};
  return std::nullopt;
}

void sema::Sema::CheckAnnotationDefinition(const lex::Node &Declaration) {
  using K = lex::TokenKind;
  const auto Key = CurrentModule.empty()
                       ? Declaration.text
                       : CurrentModule + "." + Declaration.text;
  auto Definition = AnnotationDeclarations.find(Key);
  if (Definition == AnnotationDeclarations.end())
    return;
  auto &Info = Definition->second;
  bool HasTarget = false;
  bool HasRetention = false;
  for (const auto &Child : Declaration.children) {
    if (Child->kind != K::ast_annotation)
      continue;
    if (IsBuiltinAnnotation(Child->text, "repeatable")) {
      if (Info.Repeatable || !Child->children.empty())
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
      Info.Repeatable = true;
      continue;
    }
    if (IsBuiltinAnnotation(Child->text, "target")) {
      if (HasTarget || Child->children.empty()) {
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      HasTarget = true;
      Info.Targets = 0;
      for (const auto &Argument : Child->children) {
        if (!Argument->text.empty() || Argument->children.size() != 1 ||
            Argument->children.front()->kind != K::ast_name) {
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        const auto &Target = Argument->children.front()->text;
        if (Target == "function")
          Info.Targets |= AnnotationFunction;
        else if (Target == "class")
          Info.Targets |= AnnotationClass;
        else if (Target == "annotation")
          Info.Targets |= AnnotationDeclaration;
        else if (Target == "field")
          Info.Targets |= AnnotationField;
        else if (Target == "method")
          Info.Targets |= AnnotationMethod;
        else if (Target == "constructor")
          Info.Targets |= AnnotationConstructor;
        else if (Target == "destructor")
          Info.Targets |= AnnotationDestructor;
        else
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
      }
      continue;
    }
    if (IsBuiltinAnnotation(Child->text, "retention")) {
      if (HasRetention || Child->children.size() != 1 ||
          !Child->children.front()->text.empty() ||
          Child->children.front()->children.size() != 1 ||
          Child->children.front()->children.front()->kind != K::ast_name) {
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      HasRetention = true;
      const auto &Value = Child->children.front()->children.front()->text;
      if (Value == "source")
        Info.Retention = AnnotationRetention::Source;
      else if (Value == "compile")
        Info.Retention = AnnotationRetention::Compile;
      else
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
  }
  for (const auto &Parameter : Info.Parameters)
    if (Parameter.Default &&
        !ParseAnnotationValue(*Parameter.Default, Parameter.TypeName))
      Error(*Parameter.Default, lex::DiagnosticKind::InvalidAnnotation);
}

const sema::Sema::AnnotationInfo *
sema::Sema::ResolveAnnotation(const lex::Node &Annotation) const {
  auto Name = QualifiedName(Annotation);
  const std::string Original = Name ? *Name : Annotation.text;
  std::string Key = Original;
  if (Key.find('.') == std::string::npos && !CurrentModule.empty())
    Key = CurrentModule + "." + Key;
  const auto It = AnnotationDeclarations.find(Key);
  if (It != AnnotationDeclarations.end())
    return &It->second;
  if (Original.find('.') == std::string::npos) {
    const auto Builtin =
        AnnotationDeclarations.find("std.annotation." + Original);
    if (Builtin != AnnotationDeclarations.end())
      return &Builtin->second;
  }
  return nullptr;
}

void sema::Sema::CheckAnnotations(const lex::Node &Target) {
  using K = lex::TokenKind;
  const unsigned TargetKind =
      Target.kind == K::ast_function
          ? (Reflection.Get(*Reflection.GetId(Target)).Kind == MetaKind::Method
                 ? AnnotationMethod
                 : AnnotationFunction)
      : Target.kind == K::ast_field || Target.kind == K::ast_const_field
          ? AnnotationField
      : Target.kind == K::ast_constructor ? AnnotationConstructor
      : Target.kind == K::ast_destructor  ? AnnotationDestructor
      : Target.kind == K::ast_class       ? AnnotationClass
                                          : AnnotationDeclaration;
  std::unordered_set<const lex::Node *> Seen;
  bool SeenInterface = false;
  for (const auto &Annotation : Target.children) {
    if (Annotation->kind != K::ast_annotation)
      continue;
    const auto *Resolved = ResolveAnnotation(*Annotation);
    if (!Resolved && (IsBuiltinAnnotation(Annotation->text, "interface") ||
                      IsBuiltinAnnotation(Annotation->text, "layout") ||
                      IsBuiltinAnnotation(Annotation->text, "extern") ||
                      IsBuiltinAnnotation(Annotation->text, "callconv") ||
                      IsBuiltinAnnotation(Annotation->text, "main") ||
                      IsBuiltinAnnotation(Annotation->text, "reflect") ||
                      IsBuiltinAnnotation(Annotation->text, "inline") ||
                      IsBuiltinAnnotation(Annotation->text, "deprecated") ||
                      IsBuiltinAnnotation(Annotation->text, "target") ||
                      IsBuiltinAnnotation(Annotation->text, "repeatable") ||
                      IsBuiltinAnnotation(Annotation->text, "retention"))) {
      Error(*Annotation, lex::DiagnosticKind::UnknownAnnotation);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "interface")) {
      if (Target.kind != K::ast_class)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      if (!Annotation->children.empty() || SeenInterface)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
      SeenInterface = true;
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "final")) {
      if (Target.kind != K::ast_class)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      if (!Annotation->children.empty())
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "layout")) {
      if (Target.kind != K::ast_class)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "extern")) {
      if (Target.kind != K::ast_function)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "callconv")) {
      if (Target.kind != K::ast_function)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (Target.kind == K::ast_annotation_decl &&
        (IsBuiltinAnnotation(Annotation->text, "target") ||
         IsBuiltinAnnotation(Annotation->text, "repeatable") ||
         IsBuiltinAnnotation(Annotation->text, "retention")))
      continue;
    const auto *Info = Resolved;
    if (!Info) {
      Error(*Annotation, lex::DiagnosticKind::UnknownAnnotation);
      continue;
    }
    if ((Info->Targets & TargetKind) == 0) {
      Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (Info->Module == BuiltinAnnotationModule &&
        Info->Node->text == "reflect" && Target.kind == K::ast_const_field) {
      Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (Info->Module != CurrentModule && Info->Module != "std.annotation") {
      const auto Import = Imports.find(CurrentModule);
      if (!Info->Public || Import == Imports.end() ||
          (!Import->second.contains(Info->Module) &&
           !Import->second.contains(Info->Module + ".*"))) {
        Error(*Annotation, lex::DiagnosticKind::PrivateDeclaration);
        continue;
      }
    }
    if (!Info->Repeatable && !Seen.emplace(Info->Node).second) {
      Error(*Annotation, lex::DiagnosticKind::DuplicateAnnotation);
      continue;
    }

    std::vector<std::optional<AnnotationValue>> Values(Info->Parameters.size());
    std::size_t Positional = 0;
    bool SawNamed = false;
    bool Invalid = false;
    for (const auto &Argument : Annotation->children) {
      if (Argument->children.size() != 1) {
        Invalid = true;
        Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      std::size_t Index = Positional;
      if (!Argument->text.empty()) {
        SawNamed = true;
        const auto Parameter =
            std::find_if(Info->Parameters.begin(), Info->Parameters.end(),
                         [&](const AnnotationParameter &Value) {
                           return Value.Name == Argument->text;
                         });
        if (Parameter == Info->Parameters.end()) {
          Invalid = true;
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        Index = std::distance(Info->Parameters.begin(), Parameter);
      } else {
        if (SawNamed)
          Invalid = true;
        ++Positional;
      }
      if (Index >= Info->Parameters.size() || Values[Index]) {
        Invalid = true;
        Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      Values[Index] = ParseAnnotationValue(*Argument->children.front(),
                                           Info->Parameters[Index].TypeName);
      if (!Values[Index]) {
        Invalid = true;
        Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
      }
    }

    AnnotationInstance Instance;
    Instance.Name = Info->Module.empty()
                        ? Info->Node->text
                        : Info->Module + "." + Info->Node->text;
    for (std::size_t I = 0; I < Info->Parameters.size(); ++I) {
      if (!Values[I] && Info->Parameters[I].Default)
        Values[I] = ParseAnnotationValue(*Info->Parameters[I].Default,
                                         Info->Parameters[I].TypeName);
      if (!Values[I]) {
        Invalid = true;
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      Instance.Arguments.push_back(
          {Info->Parameters[I].Name, std::move(*Values[I])});
    }
    if (!Invalid && Info->Module == BuiltinAnnotationModule &&
        Info->Node->text == "inline" &&
        (Instance.Arguments.empty() ||
         (Instance.Arguments.front().Value.Text != "auto" &&
          Instance.Arguments.front().Value.Text != "always"))) {
      Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
      Invalid = true;
    }
    if (!Invalid && Info->Module == BuiltinAnnotationModule &&
        Info->Node->text == "inline" &&
        Instance.Arguments.front().Value.Text == "always" &&
        !std::any_of(
            Target.children.begin(), Target.children.end(),
            [](const auto &Child) { return Child->kind == K::ast_block; })) {
      Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
      Invalid = true;
    }
    if (!Invalid && Info->Retention == AnnotationRetention::Compile)
      AnnotationInstances[&Target].push_back(std::move(Instance));
  }
}

const std::vector<sema::AnnotationInstance> &
sema::Sema::GetAnnotations(const lex::Node &Node) const {
  static const std::vector<AnnotationInstance> Empty;
  const auto It = AnnotationInstances.find(&Node);
  return It == AnnotationInstances.end() ? Empty : It->second;
}
