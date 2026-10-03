#include "Front/Sema/Sema.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"

#include <algorithm>
#include <charconv>
#include <unordered_map>
#include <unordered_set>

using namespace kelyra;

namespace {
std::string MetaIntrinsicName(const lex::Node &Node) {
  using K = lex::NodeKind;
  if (Node.kind == K::ast_name)
    return Node.text;
  if (Node.kind == K::ast_member && Node.children.size() == 1) {
    const auto Parent = MetaIntrinsicName(*Node.children.front());
    return Parent.empty() ? std::string() : Parent + "." + Node.text;
  }
  return {};
}

std::string MetaIntrinsicOperation(const lex::Node &Module, std::string_view Callee) {
  using K = lex::NodeKind;
  for (const auto &Function : Module.children) {
    if (Function->kind != K::ast_function || Callee != "std.meta." + Function->text)
      continue;
    for (const auto &Part : Function->children) {
      if (Part->kind != K::ast_annotation || !IsBuiltinAnnotation(Part->text, "intrinsic"))
        continue;
      if (Part->children.empty())
        return Function->text;
      if (Part->children.size() != 1 || Part->children.front()->children.size() != 1)
        continue;
      const auto &Value = Part->children.front()->children.front()->text;
      if (Value.size() >= 2 && Value.front() == '"' && Value.back() == '"')
        return Value.substr(1, Value.size() - 2);
    }
  }
  return {};
}

std::optional<std::unordered_set<std::string>> AsmPlaceholders(std::string_view Text) {
  std::unordered_set<std::string> Result;
  for (std::size_t I = 0; I < Text.size(); ++I) {
    if (Text[I] == '}') {
      if (I + 1 == Text.size() || Text[I + 1] != '}')
        return std::nullopt;
      ++I;
      continue;
    }
    if (Text[I] != '{')
      continue;
    if (I + 1 < Text.size() && Text[I + 1] == '{') {
      ++I;
      continue;
    }
    const auto End = Text.find('}', I + 1);
    if (End == std::string_view::npos || End == I + 1)
      return std::nullopt;
    const auto Name = Text.substr(I + 1, End - I - 1);
    if (!std::all_of(Name.begin(),
                     Name.end(),
                     [](char C) {
                       return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
                              (C >= '0' && C <= '9') || C == '_';
                     }) ||
        (Name.front() >= '0' && Name.front() <= '9'))
      return std::nullopt;
    Result.emplace(Name);
    I = End;
  }
  return Result;
}
} // namespace

void sema::Sema::CheckStatement(const lex::Node &Statement, unsigned LoopDepth) {
  using K = lex::NodeKind;
  using Handler = void (Sema::*)(const lex::Node &, unsigned);
  static const std::unordered_map<K, Handler> Handlers = {
      {K::ast_block, &Sema::CheckBlockStatement},
      {K::ast_alias_decl, &Sema::CheckAliasStatement},
      {K::ast_let, &Sema::CheckLetStatement},
      {K::ast_assign, &Sema::CheckAssignStatement},
      {K::ast_expr_stmt, &Sema::CheckExpressionStatement},
      {K::ast_return, &Sema::CheckReturnStatement},
      {K::ast_if, &Sema::CheckIfStatement},
      {K::ast_when, &Sema::CheckWhenStatement},
      {K::ast_while, &Sema::CheckWhileStatement},
      {K::ast_for, &Sema::CheckForStatement},
      {K::ast_break, &Sema::CheckLoopControlStatement},
      {K::ast_continue, &Sema::CheckLoopControlStatement},
      {K::ast_asm, &Sema::CheckAsmStatement},
  };
  const auto It = Handlers.find(Statement.kind);
  if (It == Handlers.end()) {
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
    return;
  }
  const auto PreviousLoopDepth = Context.CurrentLoopDepth;
  Context.CurrentLoopDepth = LoopDepth;
  (this->*It->second)(Statement, LoopDepth);
  Context.CurrentLoopDepth = PreviousLoopDepth;
}

void sema::Sema::CheckBlockStatement(const lex::Node &Statement, unsigned LoopDepth) {
  CheckBlock(Statement, LoopDepth);
}

void sema::Sema::CheckAliasStatement(const lex::Node &Statement, unsigned) {
  if (Context.LocalTypeScopes.empty() || Statement.children.empty() ||
      std::any_of(Statement.children.begin(), Statement.children.end(), [](const auto &Child) {
        return Child->kind == lex::NodeKind::ast_public ||
               Child->kind == lex::NodeKind::ast_annotation;
      })) {
    Error(Statement, lex::DiagnosticKind::UnsupportedDeclaration);
    return;
  }
  auto &Scope = Context.LocalTypeScopes.back();
  if (Scope.contains(Statement.text) || ParseBuiltinType(Statement.text)) {
    Error(Statement, lex::DiagnosticKind::UnsupportedDeclaration);
    return;
  }
  if (std::any_of(Statement.children.begin(), Statement.children.end(), [](const auto &Child) {
        return Child->kind == lex::NodeKind::ast_generic_parameter;
      }))
    return;
  if (auto Value = CheckType(*Statement.children.back()))
    Scope.emplace(Statement.text, *Value);
}

void sema::Sema::CheckLetStatement(const lex::Node &Statement, unsigned) {
  const lex::Node *Name = Statement.children.front().get();
  if (Name->kind == lex::NodeKind::ast_binding_list) {
    auto Result = CheckExpression(*Statement.children.back());
    if (!Result)
      return;
    if (!Result->IsResults() || Result->Results.size() != Name->children.size()) {
      Error(Statement, lex::DiagnosticKind::TypeMismatch);
      return;
    }
    for (std::size_t I = 0; I < Name->children.size(); ++I) {
      const auto &Binding = *Name->children[I];
      if (!Context.Scopes.back().emplace(Binding.text, Result->Results[I]).second)
        Error(Binding, lex::DiagnosticKind::DuplicateParameter);
      Types[&Binding] = Result->Results[I];
    }
    Types[&Statement] = *Result;
    return;
  }
  const lex::Node *TypeNode = nullptr;
  const lex::Node *Initializer = nullptr;
  if (Statement.children.size() > 1) {
    const auto &Second = Statement.children[1];
    if (sema::detail::IsTypeNode(Second->kind)) {
      TypeNode = Second.get();
      if (Statement.children.size() == 3)
        Initializer = Statement.children[2].get();
    } else {
      Initializer = Second.get();
    }
  }
  auto Declared = TypeNode ? CheckType(*TypeNode) : std::nullopt;
  Context.ConstructionContext = Initializer;
  auto Initial = Initializer ? CheckExpression(*Initializer, Declared) : std::optional<Type>();
  Context.ConstructionContext = nullptr;
  const auto Result = Declared ? Declared : Initial;
  if (Result && !Initializer && !CanZeroInitialize(*Result)) {
    Error(Statement, lex::DiagnosticKind::InvalidEnumInitialization);
    return;
  }
  if (Result && (Result->IsVoid() || Result->IsResults() || (Result->IsClass() && !Initializer))) {
    Error(Statement, lex::DiagnosticKind::ClassValueOperation);
    return;
  }
  if (Result && !Result->IsRecord() && GetBitWidth(*Result) > 128) {
    Error(Statement, lex::DiagnosticKind::UnsupportedType);
    return;
  }
  if (Name && Result) {
    if (Result->IsClass() && Initializer)
      CheckTransferAccess(*Result, IsClassTemporary(*Initializer), *Initializer);
    if (!Context.Scopes.back().emplace(Name->text, *Result).second)
      Error(*Name, lex::DiagnosticKind::DuplicateParameter);
    Types[Name] = *Result;
    Types[&Statement] = *Result;
  }
}

void sema::Sema::CheckAssignStatement(const lex::Node &Statement, unsigned) {
  if (Statement.children.size() != 2) {
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
    return;
  }
  auto Target = CheckExpression(*Statement.children[0]);
  const lex::Node *TargetNode = Statement.children[0].get();
  while (TargetNode->kind == lex::NodeKind::ast_group && TargetNode->children.size() == 1)
    TargetNode = TargetNode->children.front().get();
  if (GetFunctionValue(*TargetNode) || GetConstant(*TargetNode) ||
      GetExternalConstant(*TargetNode) || GetEnumVariant(*TargetNode)) {
    Error(Statement, lex::DiagnosticKind::InvalidAssignmentTarget);
    return;
  }
  if (Statement.children[0]->kind == lex::NodeKind::ast_name &&
      Statement.children[0]->text == "this") {
    Error(Statement, lex::DiagnosticKind::InvalidAssignmentTarget);
    return;
  }
  if (Target && Target->IsVoid()) {
    Error(Statement, lex::DiagnosticKind::ClassValueOperation);
    return;
  }
  if (Target && TargetNode->kind == lex::NodeKind::ast_index &&
      Types.at(TargetNode->children.front().get()).IsReadOnlySlice()) {
    Error(Statement, lex::DiagnosticKind::InvalidAssignmentTarget);
    return;
  }
  if (Target && TargetNode->kind == lex::NodeKind::ast_member && TargetNode->text == "len" &&
      (Types.at(TargetNode->children.front().get()).IsSlice() ||
       Types.at(TargetNode->children.front().get()).IsArray())) {
    Error(Statement, lex::DiagnosticKind::InvalidAssignmentTarget);
    return;
  }
  if (Target && !Target->IsRecord() && GetBitWidth(*Target) > 128)
    Error(Statement, lex::DiagnosticKind::UnsupportedType);
  else if (Target) {
    Context.ConstructionContext = Statement.children[1].get();
    CheckExpression(*Statement.children[1], Target);
    Context.ConstructionContext = nullptr;
    if (Target->IsClass()) {
      CheckTransferAccess(*Target, IsClassTemporary(*Statement.children[1]), Statement);
      CheckTransferAccess(*Target, true, Statement);
    }
  }
}

void sema::Sema::CheckExpressionStatement(const lex::Node &Statement, unsigned) {
  if (Statement.children.size() == 1)
    CheckExpression(*Statement.children.front());
  else
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
}

void sema::Sema::CheckReturnStatement(const lex::Node &Statement, unsigned) {
  if (Context.ReturnType && Context.ReturnType->IsResults() && Statement.children.size() > 1) {
    if (Statement.children.size() != Context.ReturnType->Results.size()) {
      Error(Statement, lex::DiagnosticKind::TypeMismatch);
      return;
    }
    for (std::size_t I = 0; I < Statement.children.size(); ++I)
      CheckExpression(*Statement.children[I], Context.ReturnType->Results[I]);
    Types[&Statement] = *Context.ReturnType;
    return;
  }
  if (!Context.ReturnType && Statement.children.empty())
    return;
  if (!Context.ReturnType || Statement.children.size() != 1) {
    Error(Statement, lex::DiagnosticKind::MissingReturn);
    return;
  }
  const auto &Value = *Statement.children.front();
  Context.ConstructionContext = &Value;
  auto Actual = CheckExpression(Value, Context.ReturnType);
  Context.ConstructionContext = nullptr;
  if (Actual && Context.ReturnType->IsClass())
    CheckTransferAccess(*Context.ReturnType, IsClassTemporary(Value), Value);
}

void sema::Sema::CheckIfStatement(const lex::Node &Statement, unsigned LoopDepth) {
  CheckExpression(*Statement.children[0], Type{BuiltinType::Bool, {}});
  CheckBlock(*Statement.children[1], LoopDepth);
  if (Statement.children.size() == 3) {
    if (Statement.children[2]->kind == lex::NodeKind::ast_if)
      CheckStatement(*Statement.children[2], LoopDepth);
    else
      CheckBlock(*Statement.children[2], LoopDepth);
  }
}

std::optional<sema::MetaId> sema::Sema::ResolveMetaTarget(const lex::Node &Target) {
  using K = lex::NodeKind;
  if (Target.kind == K::ast_pointer_type || Target.kind == K::ast_array_type ||
      Target.kind == K::ast_slice_type) {
    auto Type = CheckType(Target);
    return Type ? std::optional<MetaId>(GetOrCreateMetaType(*Type)) : std::nullopt;
  }
  if (Target.kind != K::ast_type)
    return std::nullopt;
  if (const auto Element = ParseBuiltinType(Target.text))
    return GetOrCreateMetaType(Type{*Element, {}});

  const auto Visible = [&](const MetaDeclaration &Record) {
    if (Record.Kind == MetaKind::Module) {
      if (Record.Name == Context.CurrentModule)
        return true;
      const auto Import = Imports.find(Context.CurrentModule);
      return Import != Imports.end() && Import->second.contains(Record.Name);
    }
    if (Record.Module == InvalidMetaId)
      return true;
    const auto &Module = Reflection.Get(Record.Module).Name;
    if (Module == Context.CurrentModule)
      return true;
    const auto Import = Imports.find(Context.CurrentModule);
    return Record.Public && Import != Imports.end() && Import->second.contains(Module);
  };
  const auto Find = [&](std::string_view Name) -> std::optional<MetaId> {
    for (const auto &Record : Reflection.GetRecords())
      if (Record.QualifiedName == Name && Visible(Record))
        return Record.Id;
    return std::nullopt;
  };

  if (Target.text.find('.') != std::string::npos)
    return Find(Target.text);
  if (!Context.CurrentModule.empty())
    if (auto Local = Find(Context.CurrentModule + "." + Target.text))
      return Local;
  if (auto Global = Find(Target.text))
    return Global;

  std::optional<MetaId> Result;
  for (const auto &Record : Reflection.GetRecords()) {
    if (Record.Name != Target.text || !Visible(Record) || Record.Module == InvalidMetaId)
      continue;
    const auto &Module = Reflection.Get(Record.Module).Name;
    const auto Import = Imports.find(Context.CurrentModule);
    if (Import == Imports.end() || !Import->second.contains(Module))
      continue;
    if (Result)
      return std::nullopt;
    Result = Record.Id;
  }
  return Result;
}

std::optional<bool> sema::Sema::EvaluateWhen(const lex::Node &Expression) {
  auto Result = EvaluateConstant(Expression);
  return Result && Result->Type == ConstValue::Kind::Bool
             ? std::optional<bool>(Result->Text == "true")
             : std::nullopt;
}

std::optional<sema::ConstValue>
sema::Sema::EvaluateMetaIntrinsic(const lex::Node &Call, const std::vector<ConstValue> &Arguments) {
  if (!MetaModule || Call.kind != lex::NodeKind::ast_call || Call.children.empty() ||
      Arguments.empty() || Arguments.front().Type != ConstValue::Kind::Integer)
    return std::nullopt;
  const auto Operation =
      MetaIntrinsicOperation(*MetaModule, MetaIntrinsicName(*Call.children.front()));
  if (Operation.empty())
    return std::nullopt;
  MetaId Id = InvalidMetaId;
  const auto &Digits = Arguments.front().Text;
  const auto Parsed = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Id);
  if (Parsed.ec != std::errc{} || Parsed.ptr != Digits.data() + Digits.size() ||
      Id >= Reflection.GetRecords().size())
    return std::nullopt;
  const auto &Record = Reflection.Get(Id);
  const bool FunctionRecord =
      Record.Kind == MetaKind::Function || Record.Kind == MetaKind::Method ||
      Record.Kind == MetaKind::Constructor || Record.Kind == MetaKind::Destructor;
  if (Arguments.size() == 1) {
    if (Operation == "__read_public" || Operation == "meta.read_public")
      return ConstValue::Bool(Record.Public);
    if (Operation == "__is_static" && (Record.Kind == MetaKind::Field || FunctionRecord))
      return ConstValue::Bool(Record.Static);
    if (FunctionRecord) {
      if (Operation == "__is_method")
        return ConstValue::Bool(Record.Kind == MetaKind::Method);
      if (Operation == "__is_constructor")
        return ConstValue::Bool(Record.Kind == MetaKind::Constructor);
      if (Operation == "__is_destructor")
        return ConstValue::Bool(Record.Kind == MetaKind::Destructor);
    }
    if (Record.Kind == MetaKind::Class) {
      const auto *Class = GetClass(Record.QualifiedName);
      if (!Class)
        return std::nullopt;
      if (Operation == "__has_constructor")
        return ConstValue::Bool(Class->Constructor != nullptr);
      if (Operation == "__has_default_constructor")
        return ConstValue::Bool(Class->DefaultConstructible);
      if (Operation == "__has_destructor")
        return ConstValue::Bool(Class->Destructor != nullptr);
      if (Operation == "__has_base_class")
        return ConstValue::Bool(!Class->BaseName.empty());
      if (Operation == "__is_interface")
        return ConstValue::Bool(Class->IsInterface);
      if (Operation == "__is_final")
        return ConstValue::Bool(Class->Final);
    }
  }
  if (Arguments.size() != 2)
    return std::nullopt;
  if (Operation == "__has_annotation" || Operation == "meta.has_annotation") {
    if (Arguments[1].Type != ConstValue::Kind::Integer)
      return std::nullopt;
    MetaId AnnotationId = InvalidMetaId;
    const auto &Other = Arguments[1].Text;
    const auto ParsedOther =
        std::from_chars(Other.data(), Other.data() + Other.size(), AnnotationId);
    if (ParsedOther.ec != std::errc{} || ParsedOther.ptr != Other.data() + Other.size() ||
        AnnotationId >= Reflection.GetRecords().size() ||
        Reflection.Get(AnnotationId).Kind != MetaKind::Annotation)
      return std::nullopt;
    const auto &Name = Reflection.Get(AnnotationId).QualifiedName;
    return ConstValue::Bool(
        std::any_of(Record.Annotations.begin(),
                    Record.Annotations.end(),
                    [&](const AnnotationInstance &Instance) { return Instance.Name == Name; }));
  }
  if (Record.Kind != MetaKind::Class || Arguments[1].Type != ConstValue::Kind::String ||
      (Operation != "__has_member" && Operation != "__has_field" && Operation != "__has_function"))
    return std::nullopt;
  const bool SameModule =
      Record.Module == InvalidMetaId || Reflection.Get(Record.Module).Name == Context.CurrentModule;
  for (const auto ChildId : Record.Children) {
    const auto &Child = Reflection.Get(ChildId);
    if ((!SameModule && !Child.Public) || Child.Name != Arguments[1].Text)
      continue;
    const bool IsField = Child.Kind == MetaKind::Field;
    const bool IsFunction = Child.Kind == MetaKind::Method || Child.Kind == MetaKind::Constructor ||
                            Child.Kind == MetaKind::Destructor;
    if ((Operation == "__has_member" && (IsField || IsFunction)) ||
        (Operation == "__has_field" && IsField) || (Operation == "__has_function" && IsFunction))
      return ConstValue::Bool(true);
  }
  return ConstValue::Bool(false);
}

void sema::Sema::CheckWhenStatement(const lex::Node &Statement, unsigned LoopDepth) {
  if (Statement.children.size() < 2 || Statement.children.size() > 3) {
    Error(Statement, lex::DiagnosticKind::InvalidWhenCondition);
    return;
  }
  auto Condition = EvaluateWhen(*Statement.children.front());
  if (!Condition) {
    Error(*Statement.children.front(), lex::DiagnosticKind::InvalidWhenCondition);
    return;
  }
  const lex::Node *Branch = *Condition                       ? Statement.children[1].get()
                            : Statement.children.size() == 3 ? Statement.children[2].get()
                                                             : nullptr;
  WhenBranches[&Statement] = Branch;
  if (!Branch)
    return;
  if (Branch->kind == lex::NodeKind::ast_when)
    CheckStatement(*Branch, LoopDepth);
  else
    CheckBlock(*Branch, LoopDepth);
}

void sema::Sema::CheckWhileStatement(const lex::Node &Statement, unsigned LoopDepth) {
  CheckExpression(*Statement.children[0], Type{BuiltinType::Bool, {}});
  CheckBlock(*Statement.children[1], LoopDepth + 1);
}

void sema::Sema::CheckForStatement(const lex::Node &Statement, unsigned LoopDepth) {
  if (Statement.children.size() != 3 ||
      Statement.children.front()->kind != lex::NodeKind::ast_name ||
      Statement.children.back()->kind != lex::NodeKind::ast_block) {
    Error(Statement, lex::DiagnosticKind::InvalidForIterator);
    return;
  }
  const auto *PreviousConstruction = Context.ConstructionContext;
  Context.ConstructionContext = Statement.children[1].get();
  const auto Range = CheckExpression(*Statement.children[1]);
  Context.ConstructionContext = PreviousConstruction;
  if (!Range)
    return;
  const auto *Container = GetClass(Range->IsPointer() ? Range->Pointee() : *Range);
  if (!Container || Container->IsInterface || Range->PointerDepth > 1) {
    Error(*Statement.children[1], lex::DiagnosticKind::InvalidForIterator);
    return;
  }
  const auto Method = [&](const ClassInfo *Owner, std::string_view Name) -> const FunctionInfo * {
    for (; Owner; Owner = Owner->BaseName.empty() ? nullptr : GetClass(Owner->BaseName)) {
      const auto It = Functions.find(Owner->QualifiedName + "." + std::string(Name));
      if (It == Functions.end())
        continue;
      const auto &Info = It->second;
      if (Info.Static || Info.Abstract || (Info.Module != Context.CurrentModule && !Info.Public))
        return nullptr;
      return &Info;
    }
    return nullptr;
  };
  const auto *Iter = Method(Container, "iter");
  if (Iter && (Iter->Parameters.size() != 1 || !Iter->Return.IsClass())) {
    Error(Statement, lex::DiagnosticKind::InvalidForIterator);
    return;
  }
  const auto IteratorType = Iter ? Iter->Return : (Range->IsPointer() ? Range->Pointee() : *Range);
  const auto *Iterator = GetClass(IteratorType);
  const auto *Next = Method(Iterator, "next");
  if (!Next || Next->Parameters.size() != 1 || !Next->Return.IsClass()) {
    Error(Statement, lex::DiagnosticKind::InvalidForIterator);
    return;
  }
  const auto *Maybe = GetClass(Next->Return);
  if (!Maybe || Maybe->Module != "std.util.maybe" || !Maybe->Name.starts_with("Maybe__G")) {
    Error(Statement, lex::DiagnosticKind::InvalidForIterator);
    return;
  }
  const auto *HasValue = Method(Maybe, "has_value");
  const auto *Value = Method(Maybe, "value");
  if (!HasValue || !Value || HasValue->Parameters.size() != 1 ||
      HasValue->Return != Type{BuiltinType::Bool, {}} || Value->Parameters.size() != 1 ||
      Value->Return.IsVoid() || Value->Return.IsResults()) {
    Error(Statement, lex::DiagnosticKind::InvalidForIterator);
    return;
  }
  ForLoops[&Statement] = {IteratorType,
                          Value->Return,
                          Next->Return,
                          Iter ? Iter->Symbol : std::string(),
                          Next->Symbol,
                          HasValue->Symbol,
                          Value->Symbol};
  Types[Statement.children.front().get()] = Value->Return;
  Context.Scopes.emplace_back();
  Context.Scopes.back().emplace(Statement.children.front()->text, Value->Return);
  CheckBlock(*Statement.children.back(), LoopDepth + 1);
  Context.Scopes.pop_back();
}

void sema::Sema::CheckLoopControlStatement(const lex::Node &Statement, unsigned LoopDepth) {
  if (LoopDepth == 0)
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
}

void sema::Sema::CheckAsmStatement(const lex::Node &Statement, unsigned) {
  using K = lex::NodeKind;
  const auto Placeholders = AsmPlaceholders(Statement.text);
  if (!Placeholders) {
    Error(Statement, lex::DiagnosticKind::InvalidInlineAssembly);
    return;
  }
  std::unordered_map<std::string, const lex::Node *> Inputs;
  std::unordered_map<std::string, const lex::Node *> Outputs;
  std::unordered_set<std::string> Options;
  bool Invalid = false;
  for (const auto &Child : Statement.children) {
    if (Child->kind == K::ast_asm_input || Child->kind == K::ast_asm_output) {
      const auto *Type = FindName(Child->text);
      if (!Type) {
        Error(*Child, lex::DiagnosticKind::UnknownName);
        Invalid = true;
        continue;
      }
      if (Type->IsArray() || Type->IsRecord() || Type->IsClass() || Type->IsFunction() ||
          GetBitWidth(*Type) > 128) {
        Error(*Child, lex::DiagnosticKind::InvalidInlineAssembly);
        Invalid = true;
        continue;
      }
      Types[Child.get()] = *Type;
      auto &Bindings = Child->kind == K::ast_asm_input ? Inputs : Outputs;
      if (!Bindings.emplace(Child->text, Child.get()).second) {
        Error(*Child, lex::DiagnosticKind::InvalidInlineAssembly);
        Invalid = true;
      }
      continue;
    }
    static const std::unordered_set<std::string> Supported = {
        "nomem", "nostack", "preserves_flags", "intel", "clobber"};
    if (Child->kind != K::ast_asm_option || !Supported.contains(Child->text) ||
        !Options.emplace(Child->text).second ||
        (Child->text == "clobber" && Child->children.empty()) ||
        (Child->text != "clobber" && !Child->children.empty())) {
      Error(*Child, lex::DiagnosticKind::InvalidInlineAssembly);
      Invalid = true;
    }
  }
  for (const auto &Placeholder : *Placeholders)
    if (!Inputs.contains(Placeholder) && !Outputs.contains(Placeholder)) {
      Error(Statement, lex::DiagnosticKind::InvalidInlineAssembly);
      Invalid = true;
    }
  for (const auto &[Name, Input] : Inputs) {
    const auto Output = Outputs.find(Name);
    if (Output != Outputs.end() && !Input->children.empty() && !Output->second->children.empty() &&
        Input->children.front()->text != Output->second->children.front()->text) {
      Error(Statement, lex::DiagnosticKind::InvalidInlineAssembly);
      Invalid = true;
    }
  }
  for (const auto &[Name, Input] : Inputs)
    if (Input->children.empty() && !Placeholders->contains(Name)) {
      Error(*Input, lex::DiagnosticKind::InvalidInlineAssembly);
      Invalid = true;
    }
  for (const auto &[Name, Output] : Outputs)
    if (Output->children.empty() && !Placeholders->contains(Name)) {
      Error(*Output, lex::DiagnosticKind::InvalidInlineAssembly);
      Invalid = true;
    }
  if (!Invalid)
    Types[&Statement] = Type{BuiltinType::Bool, {}};
}
