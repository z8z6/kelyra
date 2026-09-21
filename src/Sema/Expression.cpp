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
  if (Node.kind == K::ast_name || Node.kind == K::ast_index ||
      Node.kind == K::ast_member)
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
      {K::ast_member, &Sema::CheckMemberExpression},
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
  if (Expression.text == "this" && CurrentConstructor && !CheckingFieldBase &&
      InitializedFields < GetClass(CurrentClass)->Fields.size()) {
    Error(Expression, lex::DiagnosticKind::UninitializedField);
    return std::nullopt;
  }
  const auto *Result = FindName(Expression.text);
  if (!Result && !CurrentClass.empty()) {
    const auto *Class = GetClass(CurrentClass);
    if (Class)
      for (std::size_t I = 0; I < Class->Fields.size(); ++I)
        if (Class->Fields[I].Name == Expression.text) {
          if (CurrentConstructor && I >= InitializedFields &&
              InitializingTarget != &Expression) {
            Error(Expression, lex::DiagnosticKind::UninitializedField);
            return std::nullopt;
          }
          FieldReferences[&Expression] = {CurrentClass, I};
          return FinishExpression(Expression, Class->Fields[I].Value, Expected);
        }
  }
  if (!Result) {
    return CheckFunctionValue(Expression, Expected);
  }
  return FinishExpression(Expression, *Result, Expected);
}

std::optional<sema::Type>
sema::Sema::CheckMemberExpression(const lex::Node &Expression,
                                  std::optional<Type> Expected, bool) {
  if (Expression.children.size() != 1) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  const lex::Node *Root = &Expression;
  while (Root->kind == lex::TokenKind::ast_member)
    Root = Root->children.front().get();
  if (Root->kind == lex::TokenKind::ast_name && !FindName(Root->text)) {
    const auto *Class = GetClass(CurrentClass);
    bool Field = false;
    if (Class)
      for (const auto &Member : Class->Fields)
        Field |= Member.Name == Root->text;
    if (!Field)
      return CheckFunctionValue(Expression, Expected);
  }
  const bool PreviousFieldBase = CheckingFieldBase;
  CheckingFieldBase =
      Expression.children.front()->kind == lex::TokenKind::ast_name &&
      Expression.children.front()->text == "this";
  auto Base = CheckExpression(*Expression.children.front());
  CheckingFieldBase = PreviousFieldBase;
  if (!Base)
    return std::nullopt;
  const auto *Class = GetClass(*Base);
  if (!Class && Base->IsPointer()) {
    auto Pointee = *Base;
    --Pointee.PointerDepth;
    Class = GetClass(Pointee);
  }
  if (!Class || Base->PointerDepth > 1 || Base->IsArray()) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  for (std::size_t I = 0; I < Class->Fields.size(); ++I) {
    const auto &Field = Class->Fields[I];
    if (Field.Name != Expression.text)
      continue;
    const auto &Receiver = *Expression.children.front();
    if (CurrentConstructor && Receiver.kind == lex::TokenKind::ast_name &&
        Receiver.text == "this" && I >= InitializedFields &&
        InitializingTarget != &Expression) {
      Error(Expression, lex::DiagnosticKind::UninitializedField);
      return std::nullopt;
    }
    if (Class->Module != CurrentModule && (!Class->Public || !Field.Public)) {
      Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      return std::nullopt;
    }
    FieldReferences[&Expression] = {Class->QualifiedName, I};
    return FinishExpression(Expression, Field.Value, Expected);
  }
  Error(Expression, lex::DiagnosticKind::UnknownName);
  return std::nullopt;
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
  if (Result.IsArray() || Result.IsPointer() || Result.IsRecord() ||
      Result.IsClass() || Result.IsVoid() || Result.IsResults() ||
      Result.IsFunction()) {
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
  if (!Base->IsArray() || Index->IsArray() || Index->IsPointer() ||
      !IsInteger(Index->Element)) {
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
  using K = lex::TokenKind;
  const auto &Callee = *Expression.children.front();
  const auto Name = QualifiedName(Callee);
  const auto LocalKey = [&](std::string_view Value) {
    return CurrentModule.empty() ? std::string(Value)
                                 : CurrentModule + "." + std::string(Value);
  };
  const auto Imported = [&](std::string_view Module) {
    const auto It = Imports.find(CurrentModule);
    return Module == CurrentModule ||
           (It != Imports.end() &&
            (It->second.contains(std::string(Module)) ||
             It->second.contains(std::string(Module) + ".*")));
  };
  const auto IsLocal = [&](std::string_view Value) {
    if (FindName(Value))
      return true;
    const auto *Class = GetClass(CurrentClass);
    return Class &&
           std::any_of(Class->Fields.begin(), Class->Fields.end(),
                       [&](const auto &Field) { return Field.Name == Value; });
  };
  const lex::Node *Root = &Callee;
  while (Root->kind == K::ast_member)
    Root = Root->children.front().get();
  const bool ValueRoot = Root->kind != K::ast_name || IsLocal(Root->text);
  if (ValueRoot && Callee.kind != K::ast_member)
    return CheckIndirectCall(Expression, Expected);
  std::string Key = Name.value_or("");
  if (Name && Name->find('.') == std::string::npos)
    Key = LocalKey(*Name);
  const auto *Class = !ValueRoot ? GetClass(Key) : nullptr;
  if (Class) {
    if (ConstructionContext != &Expression) {
      Error(Expression, lex::DiagnosticKind::ClassValueOperation);
      return std::nullopt;
    }
    const auto Function = Functions.find(Key + ".init");
    const bool Explicit = Function != Functions.end();
    if (!Imported(Class->Module) ||
        (Class->Module != CurrentModule &&
         (!Class->Public || (Explicit && !Function->second.Public)))) {
      Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      return std::nullopt;
    }
    const auto ArgumentCount = Expression.children.size() - 1;
    if (Explicit) {
      if (ArgumentCount + 1 != Function->second.Parameters.size()) {
        Error(Expression, lex::DiagnosticKind::TypeMismatch);
        return std::nullopt;
      }
      for (std::size_t I = 0; I < ArgumentCount; ++I)
        CheckExpression(*Expression.children[I + 1],
                        Function->second.Parameters[I + 1]);
    } else if (ArgumentCount != 0) {
      // The generated default constructor takes no arguments.
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
      return std::nullopt;
    }
    Type Result{BuiltinType::Class, {}};
    Result.ClassName = Class->QualifiedName;
    ConstructorCalls[&Expression] = Result.ClassName;
    Callees[&Expression] =
        Explicit ? Function->second.Symbol : Class->ConstructorSymbol;
    return FinishExpression(Expression, Result, Expected);
  }

  bool MethodCall = false;
  if (Callee.kind == K::ast_member && ValueRoot) {
    auto Receiver = CheckExpression(*Callee.children.front());
    if (!Receiver)
      return std::nullopt;
    Class = GetClass(*Receiver);
    if (!Class || Receiver->PointerDepth > 1 || Receiver->IsArray()) {
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
      return std::nullopt;
    }
    if (Class->Module != CurrentModule && !Class->Public) {
      Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      return std::nullopt;
    }
    for (const auto &Field : Class->Fields)
      if (Field.Name == Callee.text)
        return CheckIndirectCall(Expression, Expected);
    if (Callee.text == "init" || Callee.text == "deinit") {
      Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
      return std::nullopt;
    }
    Key = Class->QualifiedName + "." + Callee.text;
    MethodCall = true;
  } else if (!Name || ValueRoot) {
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }

  auto Function = Functions.find(Key);
  if (!MethodCall && Name && Name->find('.') == std::string::npos) {
    if (Function == Functions.end()) {
      const auto Import = Imports.find(CurrentModule);
      if (Import != Imports.end())
        for (const auto &Module : Import->second) {
          if (!Module.ends_with(".*"))
            continue;
          const auto Candidate =
              Functions.find(Module.substr(0, Module.size() - 2) + "." + *Name);
          if (Candidate == Functions.end() || !Candidate->second.Public ||
              !Candidate->second.OwnerClass.empty())
            continue;
          if (Function != Functions.end()) {
            Error(Expression, lex::DiagnosticKind::AmbiguousName);
            return std::nullopt;
          }
          Function = Candidate;
        }
    }
    if (!CurrentClass.empty()) {
      const auto Method = Functions.find(CurrentClass + "." + *Name);
      if (Method != Functions.end()) {
        if (*Name == "init" || *Name == "deinit") {
          Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
          return std::nullopt;
        }
        if (Function != Functions.end()) {
          Error(Expression, lex::DiagnosticKind::AmbiguousName);
          return std::nullopt;
        }
        if (CurrentConstructor &&
            InitializedFields < GetClass(CurrentClass)->Fields.size()) {
          Error(Expression, lex::DiagnosticKind::UninitializedField);
          return std::nullopt;
        }
        Function = Method;
        MethodCall = true;
      }
    }
  }
  if (Function == Functions.end()) {
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }
  const auto &Info = Function->second;
  if (!Info.OwnerClass.empty() && !MethodCall) {
    Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
    return std::nullopt;
  }
  if (!Imported(Info.Module) ||
      (Info.Module != CurrentModule && !Info.Public)) {
    Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
    return std::nullopt;
  }
  const auto ArgumentCount = Expression.children.size() - 1;
  const auto ParameterOffset = MethodCall ? 1u : 0u;
  if ((!Info.Variadic &&
       ArgumentCount + ParameterOffset != Info.Parameters.size()) ||
      (Info.Variadic &&
       ArgumentCount + ParameterOffset < Info.Parameters.size())) {
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  std::vector<Type> Arguments;
  for (std::size_t I = ParameterOffset; I < Info.Parameters.size(); ++I)
    if (auto Value = CheckExpression(
            *Expression.children[I + 1 - ParameterOffset], Info.Parameters[I]))
      Arguments.push_back(*Value);
  for (std::size_t I = Info.Parameters.size() - ParameterOffset;
       I < ArgumentCount; ++I)
    if (auto Value = CheckExpression(*Expression.children[I + 1])) {
      if (Value->IsClass() || Value->IsVoid() || Value->IsResults() ||
          Value->IsFunction() || Value->Element == BuiltinType::Class)
        Error(*Expression.children[I + 1],
              lex::DiagnosticKind::ClassValueOperation);
      Arguments.push_back(*Value);
    }

  const bool NeedsWrapper =
      Info.External &&
      (Info.Variadic || Info.Return.IsRecord() ||
       std::any_of(Arguments.begin(), Arguments.end(),
                   [](const Type &Type) { return Type.IsRecord(); }));
  if (NeedsWrapper && Arguments.size() == ArgumentCount) {
    CWrapper Wrapper;
    Wrapper.Name = "kelyra_c_thunk_" + SymbolPrefix + "_" +
                   std::to_string(CWrappers.size());
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
    else if (!Info.Return.IsVoid())
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
  if (MethodCall)
    MethodCalls.insert(&Expression);
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
    const lex::Node *Operand = Expression.children.front().get();
    while (Operand->kind == lex::TokenKind::ast_group)
      Operand = Operand->children.front().get();
    if (Operand->kind == lex::TokenKind::ast_name && Operand->text == "this") {
      Error(Expression, lex::DiagnosticKind::InvalidAssignmentTarget);
      return std::nullopt;
    }
    if (!IsAddressable(*Expression.children.front())) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    auto Result = CheckExpression(*Expression.children.front());
    if (GetFunctionValue(*Operand)) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
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
    if (Lhs->IsArray() || Lhs->IsClass() || Lhs->IsRecord() || Lhs->IsVoid() ||
        Lhs->IsResults() || Lhs->IsFunction() ||
        (Ordered && !IsNumeric(Lhs->Element))) {
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
