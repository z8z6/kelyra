#include "Front/Sema/ConstEval.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"

#include <algorithm>

using namespace kelyra;
using K = lex::NodeKind;

namespace {
std::optional<llvm::APInt> ParseInteger(std::string_view Text) {
  const bool Negative = Text.starts_with('-');
  if (Negative)
    Text.remove_prefix(1);
  if (Text.empty() ||
      !std::all_of(Text.begin(), Text.end(), [](char C) { return C >= '0' && C <= '9'; }) ||
      llvm::APInt::getBitsNeeded(llvm::StringRef(Text), 10) > 255)
    return std::nullopt;
  llvm::APInt Result(256, llvm::StringRef(Text), 10);
  return Negative ? -Result : Result;
}

std::string Decimal(const llvm::APInt &Value) {
  llvm::SmallString<48> Buffer;
  Value.toString(Buffer, 10, true);
  return std::string(Buffer);
}

std::optional<std::string> Unquote(std::string_view Text) {
  if (Text.size() < 2 || Text.front() != '"' || Text.back() != '"')
    return std::nullopt;
  std::string Result;
  for (std::size_t I = 1; I + 1 < Text.size(); ++I) {
    char Part = Text[I];
    if (Part == '\\' && I + 2 < Text.size()) {
      Part = Text[++I];
      if (Part == 'n')
        Part = '\n';
      else if (Part == 'r')
        Part = '\r';
      else if (Part == 't')
        Part = '\t';
      else if (Part == '0')
        Part = '\0';
    }
    Result.push_back(Part);
  }
  return Result;
}
} // namespace

std::string sema::ConstValue::LiteralSpelling() const {
  if (Type != Kind::String)
    return Text;
  std::string Result = "\"";
  for (const char Part : Text) {
    if (Part == '\n')
      Result += "\\n";
    else if (Part == '\r')
      Result += "\\r";
    else if (Part == '\t')
      Result += "\\t";
    else if (Part == '\0')
      Result += "\\0";
    else if (Part == '"' || Part == '\\') {
      Result += '\\';
      Result += Part;
    } else
      Result += Part;
  }
  Result += '"';
  return Result;
}

sema::ConstEvaluator::ConstEvaluator(Resolver Resolve, FunctionResolver FindFunction,
                                     Intrinsic CallIntrinsic, Property ReadProperty)
    : Resolve(std::move(Resolve)), FindFunction(std::move(FindFunction)),
      CallIntrinsic(std::move(CallIntrinsic)), ReadProperty(std::move(ReadProperty)) {}

std::optional<sema::ConstValue> sema::ConstEvaluator::Fail(const lex::Node &Node) {
  if (!FailedAt)
    FailedAt = &Node;
  return std::nullopt;
}

std::optional<sema::ConstValue> sema::ConstEvaluator::Evaluate(const lex::Node &Node) {
  Steps = 0;
  Depth = 0;
  FailedAt = nullptr;
  Scopes.clear();
  return Expression(Node);
}

std::optional<sema::ConstValue> sema::ConstEvaluator::Expression(const lex::Node &Node) {
  if (++Steps > 100000)
    return Fail(Node);
  if (Node.kind == K::ast_literal) {
    if (Node.text == "true" || Node.text == "false")
      return ConstValue::Bool(Node.text == "true");
    if (auto String = Unquote(Node.text))
      return ConstValue::String(std::move(*String));
    if (auto Integer = ParseInteger(Node.text))
      return ConstValue::Integer(Decimal(*Integer));
    if (Node.text.find_first_of(".eE") != std::string::npos)
      return ConstValue{ConstValue::Kind::Float, Node.text};
    return Fail(Node);
  }
  if (Node.kind == K::ast_group && Node.children.size() == 1)
    return Expression(*Node.children.front());
  if (Node.kind == K::ast_block_expr || Node.kind == K::ast_meta_block)
    return Block(Node, true);
  if (Node.kind == K::ast_name) {
    for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope)
      if (auto Found = Scope->find(Node.text); Found != Scope->end())
        return Found->second;
  }
  if (Node.kind == K::ast_member && Node.children.size() == 1) {
    if (Resolve)
      if (auto Value = Resolve(Node))
        return Value;
    if (Node.children.front()->kind == K::ast_meta && FindFunction)
      if (const auto *Declaration = FindFunction(Node)) {
        auto Receiver = Expression(*Node.children.front());
        if (Receiver && Receiver->Type == ConstValue::Kind::Symbol)
          return Function(*Declaration, {*Receiver});
      }
    auto Base = Expression(*Node.children.front());
    if (Base && Base->Type == ConstValue::Kind::Symbol) {
      if (Node.text == "id")
        return ConstValue::Integer(Base->Text);
      if (ReadProperty)
        if (auto Value = ReadProperty(*Base, Node.text))
          return Value;
    }
  }
  if (Node.kind == K::ast_unary && Node.children.size() == 1) {
    auto Value = Expression(*Node.children.front());
    if (!Value)
      return std::nullopt;
    if (Node.text == "!" && Value->Type == ConstValue::Kind::Bool)
      return ConstValue::Bool(Value->Text != "true");
    if (Value->Type == ConstValue::Kind::Integer && (Node.text == "+" || Node.text == "-")) {
      auto Integer = ParseInteger(Value->Text);
      if (Integer)
        return ConstValue::Integer(Decimal(Node.text == "-" ? -*Integer : *Integer));
    }
    return Fail(Node);
  }
  if (Node.kind == K::ast_binary && Node.children.size() == 2) {
    auto Left = Expression(*Node.children.front());
    if (!Left)
      return std::nullopt;
    if (Left->Type == ConstValue::Kind::Bool && Node.text == "&&" && Left->Text == "false")
      return ConstValue::Bool(false);
    if (Left->Type == ConstValue::Kind::Bool && Node.text == "||" && Left->Text == "true")
      return ConstValue::Bool(true);
    auto Right = Expression(*Node.children.back());
    if (!Right || Left->Type != Right->Type)
      return Fail(Node);
    if (Left->Type == ConstValue::Kind::Float)
      return Fail(Node);
    if (Node.text == "==" || Node.text == "!=")
      return ConstValue::Bool((Left->Text == Right->Text) == (Node.text == "=="));
    if (Left->Type == ConstValue::Kind::Bool) {
      if (Node.text == "&&")
        return ConstValue::Bool(Right->Text == "true");
      if (Node.text == "||")
        return ConstValue::Bool(Right->Text == "true");
    }
    if (Left->Type == ConstValue::Kind::String && Node.text == "+")
      return ConstValue::String(Left->Text + Right->Text);
    if (Left->Type == ConstValue::Kind::Integer) {
      auto A = ParseInteger(Left->Text);
      auto B = ParseInteger(Right->Text);
      if (!A || !B)
        return Fail(Node);
      if (Node.text == "<")
        return ConstValue::Bool(A->slt(*B));
      if (Node.text == "<=")
        return ConstValue::Bool(A->sle(*B));
      if (Node.text == ">")
        return ConstValue::Bool(A->sgt(*B));
      if (Node.text == ">=")
        return ConstValue::Bool(A->sge(*B));
      bool Overflow = false;
      llvm::APInt Result(256, 0);
      if (Node.text == "+")
        Result = A->sadd_ov(*B, Overflow);
      else if (Node.text == "-")
        Result = A->ssub_ov(*B, Overflow);
      else if (Node.text == "*")
        Result = A->smul_ov(*B, Overflow);
      else if (Node.text == "/" && !B->isZero())
        Result = A->sdiv_ov(*B, Overflow);
      else if (Node.text == "%" && !B->isZero())
        Result = A->srem(*B);
      else
        return Fail(Node);
      if (!Overflow)
        return ConstValue::Integer(Decimal(Result));
    }
    return Fail(Node);
  }
  if (Node.kind == K::ast_call && !Node.children.empty()) {
    std::vector<ConstValue> Arguments;
    const auto &Callee = *Node.children.front();
    if (Callee.kind == K::ast_member && Callee.children.size() == 1 &&
        Callee.children.front()->kind == K::ast_meta) {
      auto Receiver = Expression(*Callee.children.front());
      if (!Receiver || Receiver->Type != ConstValue::Kind::Symbol)
        return Fail(Node);
      Arguments.push_back(*Receiver);
    }
    for (std::size_t I = 1; I < Node.children.size(); ++I) {
      auto Argument = Expression(*Node.children[I]);
      if (!Argument)
        return std::nullopt;
      Arguments.push_back(std::move(*Argument));
    }
    if (FindFunction)
      if (const auto *Declaration = FindFunction(Callee))
        return Function(*Declaration, Arguments);
    if (CallIntrinsic)
      if (auto Result = CallIntrinsic(Node, Arguments))
        return Result;
    return Fail(Node);
  }
  if (Resolve)
    if (auto Value = Resolve(Node))
      return Value;
  return Fail(Node);
}

std::optional<sema::ConstValue>
sema::ConstEvaluator::Function(const lex::Node &Declaration,
                               const std::vector<ConstValue> &Arguments) {
  if (++Depth > 64)
    return Fail(Declaration);
  const lex::Node *Body = nullptr;
  std::vector<const lex::Node *> Parameters;
  for (const auto &Part : Declaration.children) {
    if (Part->kind == K::ast_parameter)
      Parameters.push_back(Part.get());
    else if (Part->kind == K::ast_block)
      Body = Part.get();
  }
  const bool HasReceiver = Arguments.size() == Parameters.size() + 1 &&
                           Arguments.front().Type == ConstValue::Kind::Symbol;
  if (!Body || Parameters.size() + (HasReceiver ? 1 : 0) != Arguments.size()) {
    --Depth;
    return Fail(Declaration);
  }
  auto CallerScopes = std::move(Scopes);
  Scopes.clear();
  Scopes.emplace_back();
  if (HasReceiver)
    Scopes.back().emplace("id", ConstValue::Integer(Arguments.front().Text));
  for (std::size_t I = 0; I < Arguments.size(); ++I)
    if (I >= static_cast<std::size_t>(HasReceiver))
      Scopes.back().emplace(Parameters[I - HasReceiver]->text, Arguments[I]);
  auto Result = Block(*Body, false);
  Scopes = std::move(CallerScopes);
  --Depth;
  return Result;
}

std::optional<sema::ConstValue> sema::ConstEvaluator::Block(const lex::Node &Node, bool Value) {
  Scopes.emplace_back();
  std::optional<ConstValue> Result;
  for (std::size_t I = 0; I < Node.children.size(); ++I) {
    const auto &Child = *Node.children[I];
    if (Value && I + 1 == Node.children.size() && lex::IsExpressionNode(Child.kind)) {
      Result = Expression(Child);
      break;
    }
    if (!Statement(Child, Result)) {
      Scopes.pop_back();
      return std::nullopt;
    }
    if (Result)
      break;
  }
  Scopes.pop_back();
  if (Result)
    return Result;
  if (Value)
    return Fail(Node);
  return ConstValue{ConstValue::Kind::Void, {}};
}

bool sema::ConstEvaluator::Statement(const lex::Node &Node, std::optional<ConstValue> &Returned) {
  if (Node.kind == K::ast_alias_decl)
    return true;
  if (++Steps > 100000) {
    Fail(Node);
    return false;
  }
  if (Node.kind == K::ast_block) {
    auto Value = Block(Node, false);
    if (!Value)
      return false;
    if (Value->Type != ConstValue::Kind::Void)
      Returned = Value;
    return true;
  }
  if (Node.kind == K::ast_return) {
    if (Node.children.empty()) {
      Returned = ConstValue{ConstValue::Kind::Void, {}};
      return true;
    }
    if (Node.children.size() != 1)
      return false;
    Returned = Expression(*Node.children.front());
    return Returned.has_value();
  }
  if (Node.kind == K::ast_let && Node.children.size() >= 2 &&
      Node.children.front()->kind == K::ast_name) {
    auto Value = Expression(*Node.children.back());
    if (!Value)
      return false;
    Scopes.back()[Node.children.front()->text] = *Value;
    return true;
  }
  if (Node.kind == K::ast_assign && Node.children.size() == 2 &&
      Node.children.front()->kind == K::ast_name) {
    auto Value = Expression(*Node.children.back());
    if (!Value)
      return false;
    for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope)
      if (auto Found = Scope->find(Node.children.front()->text); Found != Scope->end()) {
        Found->second = *Value;
        return true;
      }
  }
  if ((Node.kind == K::ast_if || Node.kind == K::ast_when) && Node.children.size() >= 2) {
    auto Condition = Expression(*Node.children.front());
    if (!Condition || Condition->Type != ConstValue::Kind::Bool)
      return false;
    const auto Index = Condition->Text == "true" ? 1u : 2u;
    return Index >= Node.children.size() || Statement(*Node.children[Index], Returned);
  }
  if (Node.kind == K::ast_while && Node.children.size() == 2) {
    while (true) {
      auto Condition = Expression(*Node.children.front());
      if (!Condition || Condition->Type != ConstValue::Kind::Bool)
        return false;
      if (Condition->Text != "true" || Returned)
        return true;
      if (!Statement(*Node.children.back(), Returned))
        return false;
    }
  }
  if (Node.kind == K::ast_expr_stmt && Node.children.size() == 1)
    return Expression(*Node.children.front()).has_value();
  Fail(Node);
  return false;
}
