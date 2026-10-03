#pragma once

#include "Front/AST/AST.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kelyra::sema {

struct ConstValue {
  enum class Kind { Bool, Integer, Float, String, Symbol, Enum, Void } Type;
  std::string Text;
  std::string LiteralSpelling() const;

  static ConstValue Bool(bool Value) { return {Kind::Bool, Value ? "true" : "false"}; }
  static ConstValue Integer(std::string Value) { return {Kind::Integer, std::move(Value)}; }
  static ConstValue String(std::string Value) { return {Kind::String, std::move(Value)}; }
};

// The host resolves declarations and compiler primitives. Syntax, control flow,
// arithmetic, and calls to Kelyra function bodies are interpreted here.
class ConstEvaluator {
public:
  using Resolver = std::function<std::optional<ConstValue>(const lex::Node &)>;
  using FunctionResolver = std::function<const lex::Node *(const lex::Node &)>;
  using Intrinsic =
      std::function<std::optional<ConstValue>(const lex::Node &, const std::vector<ConstValue> &)>;
  using Property = std::function<std::optional<ConstValue>(const ConstValue &, std::string_view)>;

  ConstEvaluator(Resolver Resolve = {}, FunctionResolver FindFunction = {},
                 Intrinsic CallIntrinsic = {}, Property ReadProperty = {});
  std::optional<ConstValue> Evaluate(const lex::Node &Expression);
  const lex::Node *Failure() const { return FailedAt; }

private:
  Resolver Resolve;
  FunctionResolver FindFunction;
  Intrinsic CallIntrinsic;
  Property ReadProperty;
  const lex::Node *FailedAt = nullptr;
  unsigned Steps = 0;
  unsigned Depth = 0;
  std::vector<std::unordered_map<std::string, ConstValue>> Scopes;

  std::optional<ConstValue> Expression(const lex::Node &Node);
  bool Statement(const lex::Node &Node, std::optional<ConstValue> &Returned);
  std::optional<ConstValue> Block(const lex::Node &Node, bool Value);
  std::optional<ConstValue> Function(const lex::Node &Declaration,
                                     const std::vector<ConstValue> &Arguments);
  std::optional<ConstValue> Fail(const lex::Node &Node);
};

} // namespace kelyra::sema
