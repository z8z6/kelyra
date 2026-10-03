#include "Front/Sema/Sema.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

using namespace kelyra;

namespace {
std::optional<std::string> QualifiedName(const lex::Node &Node) {
  using K = lex::NodeKind;
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
  if (Name == "std.util.string.StringSlice" || Name == "std.meta.Symbol" ||
      Name == "std.meta.Type" || Name == "std.meta.Class" || Name == "std.meta.Field" ||
      Name == "std.meta.Function" || Name == "std.meta.Parameter" || Name == "std.meta.Annotation")
    return true;
  const auto Type = sema::ParseBuiltinType(Name);
  return Type && (sema::IsNumeric(*Type) || *Type == sema::BuiltinType::Bool ||
                  *Type == sema::BuiltinType::Char);
}

} // namespace

void sema::Sema::RegisterAnnotation(const lex::Node &Declaration, std::string_view Module) {
  using K = lex::NodeKind;
  if (Module != BuiltinAnnotationModule &&
      (Declaration.text == "layout" || Declaration.text == "cfg" || Declaration.text == "extern" ||
       Declaration.text == "intrinsic" || Declaration.text == "callconv" ||
       Declaration.text == "interface" || Declaration.text == "final" ||
       Declaration.text == "forward" || Declaration.text == "static" ||
       Declaration.text == "virtual" || Declaration.text == "override" ||
       Declaration.text == "main" || Declaration.text == "reflect" ||
       Declaration.text == "inline" || Declaration.text == "deprecated" ||
       Declaration.text == "target" || Declaration.text == "repeatable" ||
       Declaration.text == "retention" || Declaration.text == "meta")) {
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
        Child->children.front()->kind != K::ast_type) {
      Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
    const auto &RawTypeName = Child->children.front()->text;
    std::string TypeName = RawTypeName;
    if (!IsAnnotationType(TypeName) && RawTypeName.find('.') == std::string::npos) {
      const auto Local = std::string(Module) + "." + RawTypeName;
      if (IsAnnotationType(Local))
        TypeName = Local;
      if (const auto Import = Imports.find(std::string(Module)); Import != Imports.end()) {
        for (const auto &Imported : Import->second) {
          const auto Candidate = Imported + "." + RawTypeName;
          if (!IsAnnotationType(Candidate))
            continue;
          if (TypeName != RawTypeName && TypeName != Candidate) {
            Error(*Child, lex::DiagnosticKind::AmbiguousName);
            TypeName.clear();
            break;
          }
          TypeName = Candidate;
        }
      }
    }
    if (TypeName.empty())
      continue;
    std::optional<Type> EnumType;
    if (!IsAnnotationType(TypeName))
      EnumType = CheckType(*Child->children.front());
    if (!IsAnnotationType(TypeName) && (!EnumType || !EnumType->IsEnum())) {
      Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
    if (!Names.emplace(Child->text).second) {
      Error(*Child, lex::DiagnosticKind::DuplicateAnnotationParameter);
      continue;
    }
    const auto *Default = Child->children.size() == 2 ? Child->children.back().get() : nullptr;
    if (!Default && SawDefault)
      Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
    SawDefault |= Default != nullptr;
    if (EnumType && EnumType->IsEnum())
      TypeName = EnumType->EnumName;
    Info.Parameters.push_back({Child->text, TypeName, Default});
  }
  const auto Key = Info.Module.empty() ? Declaration.text : Info.Module + "." + Declaration.text;
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
    auto ParameterMeta =
        RegisterMetaDeclaration(*Child, MetaKind::AnnotationParameter, Module, false);
    auto &ParameterRecord = Reflection.Records[ParameterMeta];
    ParameterRecord.QualifiedName =
        Reflection.Records[*AnnotationMeta].QualifiedName + "." + Child->text;
    const auto &Parameters = AnnotationDeclarations.at(Key).Parameters;
    const auto Parameter = std::find_if(Parameters.begin(),
                                        Parameters.end(),
                                        [&](const auto &Info) { return Info.Name == Child->text; });
    if (Parameter == Parameters.end())
      continue;
    const auto &TypeName = Parameter->TypeName;
    if (TypeName == "std.util.string.StringSlice") {
      Type Slice{BuiltinType::U8, {}};
      Slice.AddSlice(true);
      ParameterRecord.Type = GetOrCreateMetaType(Slice);
    } else if (const auto EnumId = Reflection.Find(TypeName, MetaKind::Enum)) {
      ParameterRecord.Type = *EnumId;
    } else if (const auto ClassId = Reflection.Find(TypeName, MetaKind::Class)) {
      ParameterRecord.Type = *ClassId;
    } else if (const auto Element = ParseBuiltinType(TypeName)) {
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
sema::Sema::ParseAnnotationValue(const lex::Node &Expression, std::string_view ExpectedType) {
  using K = lex::NodeKind;
  if (Expression.kind == K::ast_annotation_binding) {
    if (ExpectedType != "std.annotation.Target" || Expression.children.size() != 1)
      return std::nullopt;
    return ParseAnnotationValue(*Expression.children.front(), ExpectedType);
  }
  if (const auto *Enum = GetEnum(ExpectedType)) {
    const auto Name = QualifiedName(Expression);
    if (!Name)
      return std::nullopt;
    const auto Dot = Name->rfind('.');
    if (Dot == std::string::npos)
      return std::nullopt;
    const auto Owner = Name->substr(0, Dot);
    if (Owner != Enum->QualifiedName &&
        Owner != Enum->QualifiedName.substr(Enum->QualifiedName.rfind('.') + 1))
      return std::nullopt;
    for (const auto &Variant : Enum->Variants)
      if (Variant.Name == Name->substr(Dot + 1)) {
        const auto Qualified = Enum->QualifiedName + "." + Variant.Name;
        return AnnotationValue{
            AnnotationValueKind::Enum,
            Qualified,
            Reflection.Find(Qualified, MetaKind::EnumVariant).value_or(InvalidMetaId)};
      }
    return std::nullopt;
  }
  if (ExpectedType == "std.util.string.StringSlice") {
    const auto Constant = EvaluateConstant(Expression);
    if (Constant && Constant->Type == ConstValue::Kind::String)
      return AnnotationValue{AnnotationValueKind::String, Constant->LiteralSpelling()};
    return std::nullopt;
  }
  if (ExpectedType.starts_with("std.meta.")) {
    if (Expression.kind != K::ast_meta || Expression.children.size() != 1)
      return std::nullopt;
    const auto Id = ResolveMetaTarget(*Expression.children.front());
    if (!Id)
      return std::nullopt;
    const auto &Record = Reflection.Get(*Id);
    const bool IsType = Record.Kind == MetaKind::Type || Record.Kind == MetaKind::Class ||
                        Record.Kind == MetaKind::Enum;
    const bool IsFunction = Record.Kind == MetaKind::Function || Record.Kind == MetaKind::Method ||
                            Record.Kind == MetaKind::Constructor ||
                            Record.Kind == MetaKind::Destructor;
    const bool Matches =
        ExpectedType == "std.meta.Symbol" || (ExpectedType == "std.meta.Type" && IsType) ||
        (ExpectedType == "std.meta.Class" && Record.Kind == MetaKind::Class) ||
        (ExpectedType == "std.meta.Field" && Record.Kind == MetaKind::Field) ||
        (ExpectedType == "std.meta.Function" && IsFunction) ||
        (ExpectedType == "std.meta.Parameter" && Record.Kind == MetaKind::Parameter) ||
        (ExpectedType == "std.meta.Annotation" && Record.Kind == MetaKind::Annotation);
    if (!Matches)
      return std::nullopt;
    const auto Kind = IsType ? AnnotationValueKind::Type : AnnotationValueKind::Symbol;
    return AnnotationValue{Kind, Record.Name, *Id};
  }
  const auto Element = ParseBuiltinType(ExpectedType);
  if (!Element)
    return std::nullopt;
  const auto Constant = EvaluateConstant(Expression);
  if (*Element == BuiltinType::Bool) {
    if (Constant && Constant->Type == ConstValue::Kind::Bool)
      return AnnotationValue{AnnotationValueKind::Bool, Constant->Text};
    return std::nullopt;
  }
  if (IsInteger(*Element) || *Element == BuiltinType::Char) {
    if (!Constant || Constant->Type != ConstValue::Kind::Integer)
      return std::nullopt;
    std::string_view Digits = Constant->Text;
    const bool Negative = Digits.starts_with('-');
    if (Negative)
      Digits.remove_prefix(1);
    if (!detail::FitsInteger(Digits, Type{*Element, {}}, Negative, Layout))
      return std::nullopt;
    return AnnotationValue{AnnotationValueKind::Integer, Constant->Text};
  }
  const lex::Node *Literal = &Expression;
  std::string Prefix;
  if (Expression.kind == K::ast_unary && Expression.children.size() == 1 &&
      (Expression.text == "+" || Expression.text == "-")) {
    Prefix = Expression.text;
    Literal = Expression.children.front().get();
  }
  if (Literal->kind != K::ast_literal || Literal->text.empty() || Literal->text.front() == '"')
    return std::nullopt;
  if (IsFloat(*Element))
    return AnnotationValue{AnnotationValueKind::Float, Prefix + Literal->text};
  return std::nullopt;
}

void sema::Sema::CheckAnnotationDefinition(const lex::Node &Declaration) {
  using K = lex::NodeKind;
  const auto Key = Context.CurrentModule.empty() ? Declaration.text
                                                 : Context.CurrentModule + "." + Declaration.text;
  auto Definition = AnnotationDeclarations.find(Key);
  if (Definition == AnnotationDeclarations.end())
    return;
  auto &Info = Definition->second;
  bool HasTarget = false;
  bool HasRetention = false;
  unsigned BoundClassTargets = 0;
  unsigned BoundFieldTargets = 0;
  const bool HasBody =
      std::any_of(Declaration.children.begin(), Declaration.children.end(), [](const auto &Part) {
        return Part->kind == K::ast_annotation_body;
      });
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
            (Argument->children.front()->kind != K::ast_name &&
             Argument->children.front()->kind != K::ast_member &&
             Argument->children.front()->kind != K::ast_annotation_binding)) {
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        const auto &Value = *Argument->children.front();
        const auto Variant = ParseAnnotationValue(Value, "std.annotation.Target");
        if (!Variant) {
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        std::string Target = Variant->Text.substr(Variant->Text.rfind('.') + 1);
        Target.front() =
            static_cast<char>(std::tolower(static_cast<unsigned char>(Target.front())));
        const bool Bound = Value.kind == K::ast_annotation_binding;
        if (Bound && Target == "class")
          ++BoundClassTargets;
        if (Bound && Target == "field")
          ++BoundFieldTargets;
        if ((Bound && ((Target != "class" && Target != "field") || Value.children.size() != 1 ||
                       Value.text.empty())) ||
            (!Bound && HasBody))
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
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
        else if (Target == "parameter")
          Info.Targets |= AnnotationParameterTarget;
        else if (Target == "module")
          Info.Targets |= AnnotationModule;
        else
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
      }
      continue;
    }
    if (IsBuiltinAnnotation(Child->text, "retention")) {
      if (HasRetention || Child->children.size() != 1 || !Child->children.front()->text.empty() ||
          Child->children.front()->children.size() != 1) {
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      HasRetention = true;
      const auto Value = ParseAnnotationValue(*Child->children.front()->children.front(),
                                              "std.annotation.Retention");
      if (!Value) {
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      if (Value->Text == "std.annotation.Retention.Source")
        Info.Retention = AnnotationRetention::Source;
      else if (Value->Text == "std.annotation.Retention.Compile")
        Info.Retention = AnnotationRetention::Compile;
      else
        Error(*Child, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
  }
  if (HasBody && (!HasTarget || !((Info.Targets == AnnotationClass && BoundClassTargets == 1) ||
                                  (Info.Targets == AnnotationField && BoundFieldTargets == 1))))
    Error(Declaration, lex::DiagnosticKind::InvalidAnnotation);
  for (const auto &Parameter : Info.Parameters)
    if (Parameter.Default && !ParseAnnotationValue(*Parameter.Default, Parameter.TypeName))
      Error(*Parameter.Default, lex::DiagnosticKind::InvalidAnnotation);
}

const sema::Sema::AnnotationInfo *sema::Sema::ResolveAnnotation(const lex::Node &Annotation,
                                                                bool &Ambiguous) const {
  auto Name = QualifiedName(Annotation);
  const std::string Original = Name ? *Name : Annotation.text;
  std::string Key = Original;
  if (Key.find('.') == std::string::npos && !Context.CurrentModule.empty())
    Key = Context.CurrentModule + "." + Key;
  const auto It = AnnotationDeclarations.find(Key);
  if (It != AnnotationDeclarations.end())
    return &It->second;
  if (Original.find('.') == std::string::npos) {
    const auto Builtin = AnnotationDeclarations.find("std.annotation." + Original);
    if (Builtin != AnnotationDeclarations.end())
      return &Builtin->second;
    if (const auto Import = Imports.find(Context.CurrentModule); Import != Imports.end()) {
      const AnnotationInfo *ImportedAnnotation = nullptr;
      for (const auto &Imported : Import->second) {
        const auto Found = AnnotationDeclarations.find(Imported + "." + Original);
        if (Found == AnnotationDeclarations.end())
          continue;
        if (ImportedAnnotation && ImportedAnnotation != &Found->second) {
          Ambiguous = true;
          return nullptr;
        }
        ImportedAnnotation = &Found->second;
      }
      return ImportedAnnotation;
    }
  }
  return nullptr;
}

void sema::Sema::CheckAnnotations(const lex::Node &Target) {
  using K = lex::NodeKind;
  const unsigned TargetKind =
      Target.kind == K::ast_module_decl ? AnnotationModule
      : Target.kind == K::ast_function
          ? (Reflection.Get(*Reflection.GetId(Target)).Kind == MetaKind::Method
                 ? AnnotationMethod
                 : AnnotationFunction)
      : Target.kind == K::ast_field || Target.kind == K::ast_const_field ? AnnotationField
      : Target.kind == K::ast_constructor                                ? AnnotationConstructor
      : Target.kind == K::ast_destructor                                 ? AnnotationDestructor
      : Target.kind == K::ast_parameter || Target.kind == K::ast_parameter_pack
          ? AnnotationParameterTarget
      : Target.kind == K::ast_class ? AnnotationClass
                                    : AnnotationDeclaration;
  std::unordered_set<const lex::Node *> Seen;
  bool SeenInterface = false;
  for (const auto &Annotation : Target.children) {
    if (Annotation->kind != K::ast_annotation)
      continue;
    bool Ambiguous = false;
    const auto *Resolved = ResolveAnnotation(*Annotation, Ambiguous);
    if (Ambiguous) {
      Error(*Annotation, lex::DiagnosticKind::AmbiguousName);
      continue;
    }
    if (!Resolved && (IsBuiltinAnnotation(Annotation->text, "interface") ||
                      IsBuiltinAnnotation(Annotation->text, "static") ||
                      IsBuiltinAnnotation(Annotation->text, "forward") ||
                      IsBuiltinAnnotation(Annotation->text, "layout") ||
                      IsBuiltinAnnotation(Annotation->text, "extern") ||
                      IsBuiltinAnnotation(Annotation->text, "intrinsic") ||
                      IsBuiltinAnnotation(Annotation->text, "callconv") ||
                      IsBuiltinAnnotation(Annotation->text, "main") ||
                      IsBuiltinAnnotation(Annotation->text, "reflect") ||
                      IsBuiltinAnnotation(Annotation->text, "inline") ||
                      IsBuiltinAnnotation(Annotation->text, "deprecated") ||
                      IsBuiltinAnnotation(Annotation->text, "target") ||
                      IsBuiltinAnnotation(Annotation->text, "repeatable") ||
                      IsBuiltinAnnotation(Annotation->text, "retention") ||
                      IsBuiltinAnnotation(Annotation->text, "meta"))) {
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
    if (IsBuiltinAnnotation(Annotation->text, "static")) {
      if (Target.kind != K::ast_field && Target.kind != K::ast_function)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      if (!Annotation->children.empty())
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "forward")) {
      if (Target.kind != K::ast_parameter_pack)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      if (!Annotation->children.empty())
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "layout")) {
      if (Target.kind != K::ast_class)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
    }
    if (IsBuiltinAnnotation(Annotation->text, "extern")) {
      if (Target.kind != K::ast_function)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (IsBuiltinAnnotation(Annotation->text, "intrinsic")) {
      if (Target.kind != K::ast_function && Target.kind != K::ast_class)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
    }
    if (IsBuiltinAnnotation(Annotation->text, "callconv")) {
      if (Target.kind != K::ast_function)
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
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
    if (Info->Module == BuiltinAnnotationModule && Info->Node->text == "reflect" &&
        Target.kind == K::ast_const_field) {
      Error(*Annotation, lex::DiagnosticKind::InvalidAnnotationTarget);
      continue;
    }
    if (Info->Module != Context.CurrentModule && Info->Module != "std.annotation" &&
        Annotation->AnnotationOriginModule != Info->Module) {
      const auto Import = Imports.find(Context.CurrentModule);
      if (!Info->Public || Import == Imports.end() || !Import->second.contains(Info->Module)) {
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
        const auto Parameter = std::find_if(
            Info->Parameters.begin(),
            Info->Parameters.end(),
            [&](const AnnotationParameter &Value) { return Value.Name == Argument->text; });
        if (Parameter == Info->Parameters.end()) {
          Invalid = true;
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        Index = std::distance(Info->Parameters.begin(), Parameter);
      } else {
        if (SawNamed)
          Invalid = true;
        std::optional<std::size_t> EnumIndex;
        bool AmbiguousEnum = false;
        for (std::size_t I = 0; I < Info->Parameters.size(); ++I) {
          if (Values[I])
            continue;
          const auto Candidate =
              ParseAnnotationValue(*Argument->children.front(), Info->Parameters[I].TypeName);
          if (!Candidate || Candidate->Kind != AnnotationValueKind::Enum)
            continue;
          if (EnumIndex) {
            AmbiguousEnum = true;
            break;
          }
          EnumIndex = I;
        }
        if (AmbiguousEnum) {
          Invalid = true;
          Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
          continue;
        }
        if (EnumIndex)
          Index = *EnumIndex;
      }
      if (Index >= Info->Parameters.size() || Values[Index]) {
        Invalid = true;
        Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      Values[Index] =
          ParseAnnotationValue(*Argument->children.front(), Info->Parameters[Index].TypeName);
      if (!Values[Index]) {
        Invalid = true;
        Error(*Argument, lex::DiagnosticKind::InvalidAnnotation);
      }
      if (Argument->text.empty()) {
        if (Index == Positional)
          ++Positional;
        while (Positional < Values.size() && Values[Positional])
          ++Positional;
      }
    }

    AnnotationInstance Instance;
    Instance.Name = Info->Module.empty() ? Info->Node->text : Info->Module + "." + Info->Node->text;
    for (std::size_t I = 0; I < Info->Parameters.size(); ++I) {
      if (!Values[I] && Info->Parameters[I].Default)
        Values[I] =
            ParseAnnotationValue(*Info->Parameters[I].Default, Info->Parameters[I].TypeName);
      if (!Values[I]) {
        Invalid = true;
        Error(*Annotation, lex::DiagnosticKind::InvalidAnnotation);
        continue;
      }
      Instance.Arguments.push_back({Info->Parameters[I].Name, std::move(*Values[I])});
    }
    if (!Invalid && Info->Module == BuiltinAnnotationModule && Info->Node->text == "inline" &&
        Instance.Arguments.front().Value.Text == "std.annotation.InlineMode.Always" &&
        !std::any_of(Target.children.begin(), Target.children.end(), [](const auto &Child) {
          return Child->kind == K::ast_block;
        })) {
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

bool sema::Sema::AnalyzeAnnotations(const std::vector<ModuleInput> &Modules) {
  using K = lex::NodeKind;
  for (const auto &Input : Modules) {
    Context.CurrentModule = Input.Name;
    for (const auto &Child : Input.Ast->children)
      if (Child->kind == K::ast_annotation_decl)
        CheckAnnotationDefinition(*Child);
  }

  for (const auto &Input : Modules) {
    Context.CurrentModule = Input.Name;
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_module_decl)
        continue;
      CheckAnnotations(*Child);
      for (const auto &Annotation : GetAnnotations(*Child))
        if (Annotation.Name == "std.annotation.meta")
          MetaModules.insert(Context.CurrentModule);
    }
  }

  for (const auto &Input : Modules) {
    Context.CurrentModule = Input.Name;
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind == K::ast_function || Child->kind == K::ast_annotation_decl ||
          Child->kind == K::ast_class)
        CheckAnnotations(*Child);
      if (Child->kind == K::ast_class)
        for (const auto &Member : Child->children)
          if (Member->kind != K::ast_public && Member->kind != K::ast_annotation) {
            CheckAnnotations(*Member);
            for (const auto &Parameter : Member->children)
              if (Parameter->kind == K::ast_parameter || Parameter->kind == K::ast_parameter_pack)
                CheckAnnotations(*Parameter);
          }
      if (Child->kind == K::ast_function)
        for (const auto &Parameter : Child->children)
          if (Parameter->kind == K::ast_parameter || Parameter->kind == K::ast_parameter_pack)
            CheckAnnotations(*Parameter);
    }
  }
  for (const auto &[Node, Instances] : AnnotationInstances)
    for (const auto &Instance : Instances)
      if (Instance.Name == "std.annotation.meta")
        MetaDeclarations.insert(Node);
  MetaRestrictionsReady = true;

  for (const auto &[Name, Class] : Classes) {
    if (MetaModules.contains(Class.Module) || MetaDeclarations.contains(Class.Node))
      continue;
    if (!Class.BaseName.empty()) {
      const auto Base = Classes.find(Class.BaseName);
      if (Base != Classes.end() && (MetaModules.contains(Base->second.Module) ||
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
         (!Function.OwnerClass.empty() && Classes.at(Function.OwnerClass).IsInterface)))
      Error(*Function.Node, lex::DiagnosticKind::InvalidAnnotationTarget);
    if (MetaModules.contains(Function.Module) || !Function.Node ||
        MetaDeclarations.contains(Function.Node) ||
        (!Function.OwnerClass.empty() &&
         MetaDeclarations.contains(Classes.at(Function.OwnerClass).Node)))
      continue;
    if (ContainsMetaType(Function.Return) ||
        std::any_of(Function.Parameters.begin(),
                    Function.Parameters.end(),
                    [this](const Type &Parameter) { return ContainsMetaType(Parameter); }))
      Error(*Function.Node, lex::DiagnosticKind::UnsupportedType);
  }
  for (const auto *Entry : EntrypointCandidates)
    if (MetaDeclarations.contains(Entry) ||
        std::any_of(Functions.begin(), Functions.end(), [&](const auto &Function) {
          return Function.second.Node == Entry && MetaModules.contains(Function.second.Module);
        }))
      Error(*Entry, lex::DiagnosticKind::InvalidEntrypoint);

  return Diagnostics.empty();
}
