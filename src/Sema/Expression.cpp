#include "Sema/Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

using namespace kelyra;

namespace {
bool IsScalarNumeric(const sema::Type &Type) {
  return !Type.IsArray() && !Type.IsPointer() && sema::IsNumeric(Type.Element);
}

bool IsAddressable(const lex::Node &Node) {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_name || Node.kind == K::ast_index)
    return true;
  if (Node.kind == K::ast_unary && Node.text == "*")
    return true;
  return Node.kind == K::ast_group && Node.children.size() == 1 &&
         IsAddressable(*Node.children.front());
}

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
} // namespace

std::optional<sema::Type>
sema::Sema::CheckExpression(const lex::Node &Expression,
                            std::optional<Type> Expected, bool Negated) {
  using K = lex::TokenKind;
  using Handler = std::optional<Type> (Sema::*)(const lex::Node &,
                                                std::optional<Type>, bool);
  static const std::unordered_map<K, Handler> Handlers = {
      {K::ast_name, &Sema::CheckNameExpression},
      {K::ast_literal, &Sema::CheckLiteralExpression},
      {K::ast_group, &Sema::CheckGroupExpression},
      {K::ast_index, &Sema::CheckIndexExpression},
      {K::ast_call, &Sema::CheckCallExpression},
      {K::ast_unary, &Sema::CheckUnaryExpression},
      {K::ast_binary, &Sema::CheckBinaryExpression},
      {K::ast_meta, &Sema::CheckMetaExpression},
  };
  const auto It = Handlers.find(Expression.kind);
  if (It == Handlers.end()) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  return (this->*It->second)(Expression, Expected, Negated);
}

std::optional<sema::Type>
sema::Sema::CheckMetaExpression(const lex::Node &Expression,
                                std::optional<Type>, bool) {
  Error(Expression, lex::DiagnosticKind::MetaValueInRuntimeExpression);
  return std::nullopt;
}

std::optional<sema::Type>
sema::Sema::FinishExpression(const lex::Node &Expression, Type Result,
                             std::optional<Type> Expected) {
  Types[&Expression] = Result;
  if (Expected && *Expected != Result &&
      !IsCInteropCompatible(*Expected, Result))
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckNameExpression(const lex::Node &Expression,
                                std::optional<Type> Expected, bool) {
  const auto *Result = FindName(Expression.text);
  if (!Result) {
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }
  return FinishExpression(Expression, *Result, Expected);
}

std::optional<sema::Type>
sema::Sema::CheckLiteralExpression(const lex::Node &Expression,
                                   std::optional<Type> Expected, bool Negated) {
  if (Expression.text == "true" || Expression.text == "false")
    return FinishExpression(Expression, {BuiltinType::Bool, {}}, Expected);
  if (!Expression.text.empty() && Expression.text.front() == '"') {
    Type String{BuiltinType::CChar, {}};
    String.PointerDepth = 1;
    String.BitWidth = sizeof(void *) * 8;
    String.Alignment = alignof(void *);
    String.CSpelling = "const char *";
    if (Expected &&
        (!Expected->IsPointer() || Expected->Element != BuiltinType::CChar)) {
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
      return std::nullopt;
    }
    return FinishExpression(Expression, Expected.value_or(String), Expected);
  }
  const bool Floating =
      Expression.text.find_first_of(".eE") != std::string::npos;
  const auto Result = Expected.value_or(
      Type{Floating ? BuiltinType::F64 : BuiltinType::I32, {}});
  if (Result.IsArray() || Result.IsPointer() || Result.IsRecord()) {
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  if (Floating) {
    if (!IsFloat(Result.Element)) {
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
      return std::nullopt;
    }
    if (GetBitWidth(Result) > 128) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
  } else if ((!IsInteger(Result.Element) &&
              Result.Element != BuiltinType::Char) ||
             !detail::FitsInteger(Expression.text, Result, Negated)) {
    Error(Expression, lex::DiagnosticKind::InvalidIntegerLiteral);
    return std::nullopt;
  }
  Types[&Expression] = Result;
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckGroupExpression(const lex::Node &Expression,
                                 std::optional<Type> Expected, bool) {
  if (Expression.children.size() != 1) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  auto Result = CheckExpression(*Expression.children.front(), Expected);
  if (Result)
    Types[&Expression] = *Result;
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckIndexExpression(const lex::Node &Expression,
                                 std::optional<Type> Expected, bool) {
  if (Expression.children.size() != 2) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  auto Base = CheckExpression(*Expression.children[0]);
  auto Index = CheckExpression(*Expression.children[1]);
  if (!Base || !Index)
    return std::nullopt;
  if (!Base->IsArray() || Index->IsArray() || !IsInteger(Index->Element)) {
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  return FinishExpression(Expression, Base->Indexed(), Expected);
}

std::optional<sema::Type>
sema::Sema::CheckCallExpression(const lex::Node &Expression,
                                std::optional<Type> Expected, bool) {
  if (Expression.children.empty()) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  auto Name = QualifiedName(*Expression.children.front());
  if (!Name) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  std::string Key = *Name;
  if (Name->find('.') == std::string::npos && !CurrentModule.empty())
    Key = CurrentModule + "." + *Name;
  auto Function = Functions.find(Key);
  if (Function == Functions.end() && Name->find('.') == std::string::npos) {
    const auto Import = Imports.find(CurrentModule);
    if (Import != Imports.end()) {
      for (const auto &Imported : Import->second) {
        if (!Imported.ends_with(".*"))
          continue;
        const auto Module = Imported.substr(0, Imported.size() - 2);
        const auto Candidate = Functions.find(Module + "." + *Name);
        if (Candidate == Functions.end() || !Candidate->second.Public)
          continue;
        if (Function != Functions.end()) {
          Error(Expression, lex::DiagnosticKind::AmbiguousName);
          return std::nullopt;
        }
        Function = Candidate;
      }
    }
  }
  if (Function == Functions.end()) {
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }
  const auto &Info = Function->second;
  if (Info.Module != CurrentModule) {
    const auto Import = Imports.find(CurrentModule);
    if (Import == Imports.end() ||
        (!Import->second.contains(Info.Module) &&
         !Import->second.contains(Info.Module + ".*"))) {
      Error(Expression, lex::DiagnosticKind::UnknownName);
      return std::nullopt;
    }
    if (!Info.Public) {
      Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      return std::nullopt;
    }
  }
  const auto ArgumentCount = Expression.children.size() - 1;
  if ((!Info.Variadic && ArgumentCount != Info.Parameters.size()) ||
      (Info.Variadic && ArgumentCount < Info.Parameters.size())) {
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  std::vector<Type> Arguments;
  for (std::size_t I = 0; I < Info.Parameters.size(); ++I)
    if (auto Type =
            CheckExpression(*Expression.children[I + 1], Info.Parameters[I]))
      Arguments.push_back(*Type);
  for (std::size_t I = Info.Parameters.size(); I < ArgumentCount; ++I)
    if (auto Type = CheckExpression(*Expression.children[I + 1]))
      Arguments.push_back(*Type);

  const bool NeedsWrapper =
      Info.External &&
      (Info.Variadic || Info.Return.IsRecord() ||
       std::any_of(Arguments.begin(), Arguments.end(),
                   [](const Type &Type) { return Type.IsRecord(); }));
  if (NeedsWrapper && Arguments.size() == ArgumentCount) {
    CWrapper Wrapper;
    Wrapper.Name = "kelyra_c_thunk_" + std::to_string(CWrappers.size());
    Wrapper.Return = Info.Return;
    Wrapper.ReturnByAddress = Info.Return.IsRecord();
    std::ostringstream Source;
    Source << "#include \"" << Info.External->Header << "\"\n";
    if (Wrapper.ReturnByAddress)
      Source << "void";
    else
      Source << detail::CSpelling(Info.Return);
    Source << ' ' << Wrapper.Name << '(';
    bool First = true;
    if (Wrapper.ReturnByAddress) {
      Source << "void *result";
      Type ResultPointer = Info.Return;
      ++ResultPointer.PointerDepth;
      ResultPointer.BitWidth = sizeof(void *) * 8;
      ResultPointer.Alignment = alignof(void *);
      Wrapper.Parameters.push_back(std::move(ResultPointer));
      First = false;
    }
    for (std::size_t I = 0; I < Arguments.size(); ++I) {
      if (!First)
        Source << ", ";
      First = false;
      const bool ByAddress = Arguments[I].IsRecord();
      Wrapper.ParametersByAddress.push_back(ByAddress);
      if (ByAddress) {
        Source << "void *arg" << I;
        Type Pointer = Arguments[I];
        ++Pointer.PointerDepth;
        Pointer.BitWidth = sizeof(void *) * 8;
        Pointer.Alignment = alignof(void *);
        Wrapper.Parameters.push_back(std::move(Pointer));
      } else {
        Source << detail::CSpelling(Arguments[I]) << " arg" << I;
        Wrapper.Parameters.push_back(Arguments[I]);
      }
    }
    Source << ") { ";
    if (Wrapper.ReturnByAddress)
      Source << "*(" << detail::CSpelling(Info.Return) << " *)result = ";
    else
      Source << "return ";
    Source << Info.External->Name << '(';
    for (std::size_t I = 0; I < Arguments.size(); ++I) {
      if (I)
        Source << ", ";
      if (Arguments[I].IsRecord())
        Source << "*(" << detail::CSpelling(Arguments[I]) << " *)arg" << I;
      else
        Source << "arg" << I;
    }
    Source << "); }\n";
    Wrapper.Source = Source.str();
    CWrapperCalls[&Expression] = CWrappers.size();
    CWrappers.push_back(std::move(Wrapper));
  }
  Callees[&Expression] = Info.Symbol;
  return FinishExpression(Expression, Info.Return, Expected);
}

std::optional<sema::Type>
sema::Sema::CheckUnaryExpression(const lex::Node &Expression,
                                 std::optional<Type> Expected, bool) {
  if (Expression.children.size() != 1) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  if (Expression.text == "&") {
    if (!IsAddressable(*Expression.children.front())) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    auto Result = CheckExpression(*Expression.children.front());
    if (!Result || Result->IsArray()) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    ++Result->PointerDepth;
    Result->BitWidth = sizeof(void *) * 8;
    Result->Alignment = alignof(void *);
    return FinishExpression(Expression, *Result, Expected);
  }
  if (Expression.text == "*") {
    auto Result = CheckExpression(*Expression.children.front());
    if (!Result || !Result->IsPointer()) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    --Result->PointerDepth;
    if (!Result->IsPointer()) {
      const auto External = ExternalTypes.find("c." + Result->CName);
      if (Result->Element == BuiltinType::CRecord &&
          External != ExternalTypes.end())
        *Result = External->second;
      else {
        Result->BitWidth = 0;
        Result->Alignment = 0;
      }
    }
    return FinishExpression(Expression, *Result, Expected);
  }
  if (Expression.text == "!") {
    const Type Bool{BuiltinType::Bool, {}};
    auto Operand = CheckExpression(*Expression.children.front(), Bool);
    if (!Operand)
      return std::nullopt;
    return FinishExpression(Expression, Bool, Expected);
  }
  auto Result = CheckExpression(*Expression.children.front(), Expected,
                                Expression.text == "-");
  if (!Result)
    return std::nullopt;
  if (!IsScalarNumeric(*Result) ||
      (Expression.text == "-" && !IsSignedInteger(Result->Element) &&
       !IsFloat(Result->Element)) ||
      (IsFloat(Result->Element) && GetBitWidth(*Result) > 128)) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  Types[&Expression] = *Result;
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckBinaryExpression(const lex::Node &Expression,
                                  std::optional<Type> Expected, bool) {
  if (Expression.children.size() != 2) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  const bool Logical = Expression.text == "&&" || Expression.text == "||";
  const bool Equality = Expression.text == "==" || Expression.text == "!=";
  const bool Ordered = Expression.text == "<" || Expression.text == "<=" ||
                       Expression.text == ">" || Expression.text == ">=";
  const bool Comparison = Equality || Ordered;
  const Type Bool{BuiltinType::Bool, {}};
  auto Lhs = CheckExpression(*Expression.children[0],
                             Logical      ? std::optional<Type>(Bool)
                             : Comparison ? std::nullopt
                                          : Expected);
  auto Rhs = CheckExpression(*Expression.children[1], Lhs);
  if (!Lhs || !Rhs)
    return std::nullopt;
  if (*Lhs != *Rhs)
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
  if (Logical)
    return FinishExpression(Expression, Bool, Expected);
  if (Comparison) {
    if (Lhs->IsArray() || (Ordered && !IsNumeric(Lhs->Element))) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    return FinishExpression(Expression, Bool, Expected);
  }
  if (!IsScalarNumeric(*Lhs) ||
      (IsFloat(Lhs->Element) &&
       (Expression.text == "%" || GetBitWidth(*Lhs) > 128))) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  Types[&Expression] = *Lhs;
  return Lhs;
}
