#include "Sema/Sema.h"

#include <functional>

using namespace kelyra;
using K = lex::TokenKind;

std::optional<sema::Type> sema::Sema::CheckFunctionType(const lex::Node &Node) {
  Type Result{BuiltinType::Function, {}};
  for (std::size_t I = 0; I < Node.children.size(); ++I) {
    auto Value = CheckType(*Node.children[I]);
    if (!Value)
      return std::nullopt;
    const bool IsReturn = I + 1 == Node.children.size();
    if (Value->IsClass() || Value->IsRecord() || Value->IsArray() ||
        (!IsReturn && (Value->IsVoid() || Value->IsResults())) ||
        GetBitWidth(*Value) > 128) {
      Error(*Node.children[I], lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    (IsReturn ? Result.Results : Result.Parameters).push_back(*Value);
  }
  Types[&Node] = Result;
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckFunctionValue(const lex::Node &Node,
                               std::optional<Type> Expected) {
  std::function<std::string(const lex::Node &)> NameOf =
      [&](const lex::Node &Part) {
        if (Part.kind == K::ast_name)
          return Part.text;
        if (Part.kind == K::ast_member)
          return NameOf(*Part.children.front()) + "." + Part.text;
        return std::string();
      };
  const auto Name = NameOf(Node);
  const bool Qualified = Name.find('.') != std::string::npos;
  const auto Key =
      Qualified || CurrentModule.empty() ? Name : CurrentModule + "." + Name;
  auto Function = Functions.find(Key);
  if (Function == Functions.end() && Qualified && !CurrentModule.empty())
    Function = Functions.find(CurrentModule + "." + Name);
  const auto Import = Imports.find(CurrentModule);
  if (!Qualified && Function == Functions.end() && Import != Imports.end()) {
    for (const auto &Module : Import->second) {
      if (!Module.ends_with(".*"))
        continue;
      const auto Candidate =
          Functions.find(Module.substr(0, Module.size() - 2) + "." + Name);
      if (Candidate == Functions.end() || !Candidate->second.Public ||
          !Candidate->second.OwnerClass.empty())
        continue;
      if (Function != Functions.end()) {
        Error(Node, lex::DiagnosticKind::AmbiguousName);
        return std::nullopt;
      }
      Function = Candidate;
    }
  }
  if (Function == Functions.end()) {
    Error(Node, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }
  const auto &Info = Function->second;
  if (Info.Module != CurrentModule &&
      (!Info.Public || Import == Imports.end() ||
       (!Import->second.contains(Info.Module) &&
        !Import->second.contains(Info.Module + ".*")))) {
    Error(Node, lex::DiagnosticKind::PrivateDeclaration);
    return std::nullopt;
  }
  if (Info.External || (!Info.OwnerClass.empty() && !Info.Static)) {
    Error(Node, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  Type Result{BuiltinType::Function, {}};
  Result.Parameters = Info.Parameters;
  Result.Results.push_back(Info.Return);
  for (const auto &Parameter : Info.Parameters)
    if (Parameter.IsArray() || Parameter.IsRecord() ||
        GetBitWidth(Parameter) > 128) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
  if (Info.Return.IsArray() || Info.Return.IsRecord() ||
      GetBitWidth(Info.Return) > 128) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  FunctionValues[&Node] = Info.Symbol;
  WarnIfDeprecated(Node, Info.Node);
  return FinishExpression(Node, Result, Expected);
}

std::optional<sema::Type>
sema::Sema::CheckIndirectCall(const lex::Node &Node,
                              std::optional<Type> Expected) {
  auto Function = CheckExpression(*Node.children.front());
  if (!Function)
    return std::nullopt;
  if (!Function->IsFunction() || Function->IsArray() ||
      Node.children.size() - 1 != Function->Parameters.size()) {
    Error(Node, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  for (std::size_t I = 0; I < Function->Parameters.size(); ++I)
    CheckExpression(*Node.children[I + 1], Function->Parameters[I]);
  IndirectCalls.insert(&Node);
  return FinishExpression(Node, Function->Results.front(), Expected);
}
