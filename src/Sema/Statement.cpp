#include "Sema/Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace kelyra;

namespace {
std::optional<std::unordered_set<std::string>>
AsmPlaceholders(std::string_view Text) {
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
    if (!std::all_of(Name.begin(), Name.end(),
                     [](char C) {
                       return (C >= 'a' && C <= 'z') ||
                              (C >= 'A' && C <= 'Z') ||
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

void sema::Sema::CheckStatement(const lex::Node &Statement,
                                unsigned LoopDepth) {
  using K = lex::TokenKind;
  using Handler = void (Sema::*)(const lex::Node &, unsigned);
  static const std::unordered_map<K, Handler> Handlers = {
      {K::ast_block, &Sema::CheckBlockStatement},
      {K::ast_let, &Sema::CheckLetStatement},
      {K::ast_assign, &Sema::CheckAssignStatement},
      {K::ast_expr_stmt, &Sema::CheckExpressionStatement},
      {K::ast_return, &Sema::CheckReturnStatement},
      {K::ast_if, &Sema::CheckIfStatement},
      {K::ast_when, &Sema::CheckWhenStatement},
      {K::ast_while, &Sema::CheckWhileStatement},
      {K::ast_break, &Sema::CheckLoopControlStatement},
      {K::ast_continue, &Sema::CheckLoopControlStatement},
      {K::ast_asm, &Sema::CheckAsmStatement},
  };
  const auto It = Handlers.find(Statement.kind);
  if (It == Handlers.end()) {
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
    return;
  }
  (this->*It->second)(Statement, LoopDepth);
}

void sema::Sema::CheckBlockStatement(const lex::Node &Statement,
                                     unsigned LoopDepth) {
  CheckBlock(Statement, LoopDepth);
}

void sema::Sema::CheckLetStatement(const lex::Node &Statement, unsigned) {
  const lex::Node *Name = Statement.children.front().get();
  if (Name->kind == lex::TokenKind::ast_binding_list) {
    auto Result = CheckExpression(*Statement.children.back());
    if (!Result)
      return;
    if (!Result->IsResults() ||
        Result->Results.size() != Name->children.size()) {
      Error(Statement, lex::DiagnosticKind::TypeMismatch);
      return;
    }
    for (std::size_t I = 0; I < Name->children.size(); ++I) {
      const auto &Binding = *Name->children[I];
      if (!Scopes.back().emplace(Binding.text, Result->Results[I]).second)
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
  ConstructionContext = Initializer;
  auto Initial = Initializer ? CheckExpression(*Initializer, Declared)
                             : std::optional<Type>();
  ConstructionContext = nullptr;
  const auto Result = Declared ? Declared : Initial;
  if (Result && (Result->IsVoid() || Result->IsResults() ||
                 (Result->IsClass() &&
                  (!Initializer || !GetConstructorCall(*Initializer))))) {
    Error(Statement, lex::DiagnosticKind::ClassValueOperation);
    return;
  }
  if (Result && !Result->IsRecord() && GetBitWidth(*Result) > 128) {
    Error(Statement, lex::DiagnosticKind::UnsupportedType);
    return;
  }
  if (Name && Result) {
    if (!Scopes.back().emplace(Name->text, *Result).second)
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
  if (GetFunctionValue(*Statement.children[0])) {
    Error(Statement, lex::DiagnosticKind::InvalidAssignmentTarget);
    return;
  }
  if (Statement.children[0]->kind == lex::TokenKind::ast_name &&
      Statement.children[0]->text == "this") {
    Error(Statement, lex::DiagnosticKind::InvalidAssignmentTarget);
    return;
  }
  if (Target && (Target->IsClass() || Target->IsVoid())) {
    Error(Statement, lex::DiagnosticKind::ClassValueOperation);
    return;
  }
  if (Target && !Target->IsRecord() && GetBitWidth(*Target) > 128)
    Error(Statement, lex::DiagnosticKind::UnsupportedType);
  else if (Target)
    CheckExpression(*Statement.children[1], Target);
}

void sema::Sema::CheckExpressionStatement(const lex::Node &Statement,
                                          unsigned) {
  if (Statement.children.size() == 1)
    CheckExpression(*Statement.children.front());
  else
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
}

void sema::Sema::CheckReturnStatement(const lex::Node &Statement, unsigned) {
  if (ReturnType && ReturnType->IsResults() && Statement.children.size() > 1) {
    if (Statement.children.size() != ReturnType->Results.size()) {
      Error(Statement, lex::DiagnosticKind::TypeMismatch);
      return;
    }
    for (std::size_t I = 0; I < Statement.children.size(); ++I)
      CheckExpression(*Statement.children[I], ReturnType->Results[I]);
    Types[&Statement] = *ReturnType;
    return;
  }
  if (!ReturnType && Statement.children.empty())
    return;
  if (!ReturnType || Statement.children.size() != 1) {
    Error(Statement, lex::DiagnosticKind::MissingReturn);
    return;
  }
  CheckExpression(*Statement.children.front(), ReturnType);
}

void sema::Sema::CheckIfStatement(const lex::Node &Statement,
                                  unsigned LoopDepth) {
  CheckExpression(*Statement.children[0], Type{BuiltinType::Bool, {}});
  CheckBlock(*Statement.children[1], LoopDepth);
  if (Statement.children.size() == 3) {
    if (Statement.children[2]->kind == lex::TokenKind::ast_if)
      CheckStatement(*Statement.children[2], LoopDepth);
    else
      CheckBlock(*Statement.children[2], LoopDepth);
  }
}

std::optional<sema::MetaId>
sema::Sema::ResolveMetaTarget(const lex::Node &Target) {
  using K = lex::TokenKind;
  if (Target.kind == K::ast_pointer_type || Target.kind == K::ast_array_type) {
    auto Type = CheckType(Target);
    return Type ? std::optional<MetaId>(GetOrCreateMetaType(*Type))
                : std::nullopt;
  }
  if (Target.kind != K::ast_type)
    return std::nullopt;
  if (const auto Element = ParseBuiltinType(Target.text))
    return GetOrCreateMetaType(Type{*Element, {}});

  const auto Visible = [&](const MetaDeclaration &Record) {
    if (Record.Kind == MetaKind::Module) {
      if (Record.Name == CurrentModule)
        return true;
      const auto Import = Imports.find(CurrentModule);
      return Import != Imports.end() &&
             (Import->second.contains(Record.Name) ||
              Import->second.contains(Record.Name + ".*"));
    }
    if (Record.Module == InvalidMetaId)
      return true;
    const auto &Module = Reflection.Get(Record.Module).Name;
    if (Module == CurrentModule)
      return true;
    const auto Import = Imports.find(CurrentModule);
    return Record.Public && Import != Imports.end() &&
           (Import->second.contains(Module) ||
            Import->second.contains(Module + ".*"));
  };
  const auto Find = [&](std::string_view Name) -> std::optional<MetaId> {
    for (const auto &Record : Reflection.GetRecords())
      if (Record.QualifiedName == Name && Visible(Record))
        return Record.Id;
    return std::nullopt;
  };

  if (Target.text.find('.') != std::string::npos)
    return Find(Target.text);
  if (!CurrentModule.empty())
    if (auto Local = Find(CurrentModule + "." + Target.text))
      return Local;
  if (auto Global = Find(Target.text))
    return Global;

  std::optional<MetaId> Result;
  for (const auto &Record : Reflection.GetRecords()) {
    if (Record.Name != Target.text || !Visible(Record) ||
        Record.Module == InvalidMetaId)
      continue;
    const auto &Module = Reflection.Get(Record.Module).Name;
    const auto Import = Imports.find(CurrentModule);
    if (Import == Imports.end() || !Import->second.contains(Module + ".*"))
      continue;
    if (Result)
      return std::nullopt;
    Result = Record.Id;
  }
  return Result;
}

std::optional<bool> sema::Sema::EvaluateWhen(const lex::Node &Expression) {
  using K = lex::TokenKind;
  if (Expression.kind == K::ast_literal) {
    if (Expression.text == "true")
      return true;
    if (Expression.text == "false")
      return false;
    return std::nullopt;
  }
  if (Expression.kind == K::ast_group && Expression.children.size() == 1)
    return EvaluateWhen(*Expression.children.front());
  if (Expression.kind == K::ast_unary && Expression.text == "!" &&
      Expression.children.size() == 1) {
    auto Value = EvaluateWhen(*Expression.children.front());
    return Value ? std::optional<bool>(!*Value) : std::nullopt;
  }
  if (Expression.kind == K::ast_binary && Expression.children.size() == 2 &&
      (Expression.text == "&&" || Expression.text == "||" ||
       Expression.text == "==" || Expression.text == "!=")) {
    auto Left = EvaluateWhen(*Expression.children.front());
    if (!Left)
      return std::nullopt;
    if (Expression.text == "&&" && !*Left)
      return false;
    if (Expression.text == "||" && *Left)
      return true;
    auto Right = EvaluateWhen(*Expression.children.back());
    if (!Right)
      return std::nullopt;
    if (Expression.text == "&&")
      return *Left && *Right;
    if (Expression.text == "||")
      return *Left || *Right;
    return Expression.text == "==" ? *Left == *Right : *Left != *Right;
  }
  if (Expression.kind == K::ast_member && Expression.text == "is_public" &&
      Expression.children.size() == 1 &&
      Expression.children.front()->kind == K::ast_meta &&
      Expression.children.front()->children.size() == 1) {
    auto Id = ResolveMetaTarget(*Expression.children.front()->children.front());
    return Id ? std::optional<bool>(Reflection.Get(*Id).Public) : std::nullopt;
  }
  if (Expression.kind != K::ast_call || Expression.children.size() != 2)
    return std::nullopt;
  const auto &Member = *Expression.children.front();
  if (Member.kind != K::ast_member || Member.text != "has_annotation" ||
      Member.children.size() != 1 ||
      Member.children.front()->kind != K::ast_meta ||
      Member.children.front()->children.size() != 1)
    return std::nullopt;
  auto Id = ResolveMetaTarget(*Member.children.front()->children.front());
  const auto *Annotation = ResolveAnnotation(*Expression.children.back());
  if (!Id || !Annotation)
    return std::nullopt;
  if (Annotation->Module != CurrentModule) {
    const auto Import = Imports.find(CurrentModule);
    if (!Annotation->Public || Import == Imports.end() ||
        (!Import->second.contains(Annotation->Module) &&
         !Import->second.contains(Annotation->Module + ".*")))
      return std::nullopt;
  }
  const auto Name = Annotation->Module.empty()
                        ? Annotation->Node->text
                        : Annotation->Module + "." + Annotation->Node->text;
  const auto &Annotations = Reflection.Get(*Id).Annotations;
  return std::any_of(Annotations.begin(), Annotations.end(),
                     [&](const AnnotationInstance &Instance) {
                       return Instance.Name == Name;
                     });
}

void sema::Sema::CheckWhenStatement(const lex::Node &Statement,
                                    unsigned LoopDepth) {
  if (Statement.children.size() < 2 || Statement.children.size() > 3) {
    Error(Statement, lex::DiagnosticKind::InvalidWhenCondition);
    return;
  }
  auto Condition = EvaluateWhen(*Statement.children.front());
  if (!Condition) {
    Error(*Statement.children.front(),
          lex::DiagnosticKind::InvalidWhenCondition);
    return;
  }
  const lex::Node *Branch = *Condition ? Statement.children[1].get()
                            : Statement.children.size() == 3
                                ? Statement.children[2].get()
                                : nullptr;
  WhenBranches[&Statement] = Branch;
  if (!Branch)
    return;
  if (Branch->kind == lex::TokenKind::ast_when)
    CheckStatement(*Branch, LoopDepth);
  else
    CheckBlock(*Branch, LoopDepth);
}

void sema::Sema::CheckWhileStatement(const lex::Node &Statement,
                                     unsigned LoopDepth) {
  CheckExpression(*Statement.children[0], Type{BuiltinType::Bool, {}});
  CheckBlock(*Statement.children[1], LoopDepth + 1);
}

void sema::Sema::CheckLoopControlStatement(const lex::Node &Statement,
                                           unsigned LoopDepth) {
  if (LoopDepth == 0)
    Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
}

void sema::Sema::CheckAsmStatement(const lex::Node &Statement, unsigned) {
  using K = lex::TokenKind;
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
      if (Type->IsArray() || Type->IsRecord() || Type->IsClass() ||
          Type->IsFunction() || GetBitWidth(*Type) > 128) {
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
    if (Output != Outputs.end() && !Input->children.empty() &&
        !Output->second->children.empty() &&
        Input->children.front()->text !=
            Output->second->children.front()->text) {
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
