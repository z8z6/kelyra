#include "Sema/Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <unordered_map>

using namespace kelyra;

namespace {
bool IsScalarNumeric(const sema::Type &Type) {
  return !Type.IsArray() && !Type.IsSlice() && !Type.IsPointer() &&
         sema::IsNumeric(Type.Element);
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
      {K::ast_slice, &Sema::CheckSliceExpression},
      {K::ast_member, &Sema::CheckMemberExpression},
      {K::ast_call, &Sema::CheckCallExpression},
      {K::ast_unary, &Sema::CheckUnaryExpression},
      {K::ast_binary, &Sema::CheckBinaryExpression},
      {K::ast_cast, &Sema::CheckCastExpression},
      {K::ast_meta, &Sema::CheckMetaExpression},
      {K::ast_meta_block, &Sema::CheckMetaBlockExpression},
      {K::ast_block_expr, &Sema::CheckBlockExpression},
      {K::ast_match, &Sema::CheckMatchExpression},
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
                                std::optional<Type> Expected, bool) {
  if (CurrentMetaContext && Expression.children.size() == 1) {
    const auto Id = ResolveMetaTarget(*Expression.children.front());
    if (Id) {
      const auto Kind = Reflection.Get(*Id).Kind;
      const bool FunctionRecord = Kind == MetaKind::Function ||
                                  Kind == MetaKind::Method ||
                                  Kind == MetaKind::Constructor ||
                                  Kind == MetaKind::Destructor;
      Type Result{BuiltinType::Class, {}};
      Result.ClassName = Kind == MetaKind::Class ? "std.meta.Class"
                         : Kind == MetaKind::Field ? "std.meta.Field"
                         : FunctionRecord          ? "std.meta.Function"
                         : Kind == MetaKind::Annotation
                             ? "std.meta.Annotation"
                             : "std.meta.Symbol";
      if (GetClass(Result.ClassName))
        return FinishExpression(Expression, Result, Expected);
    }
  }
  Error(Expression, lex::DiagnosticKind::MetaValueInRuntimeExpression);
  return std::nullopt;
}

std::optional<sema::Type>
sema::Sema::CheckMetaBlockExpression(const lex::Node &Expression,
                                     std::optional<Type> Expected, bool) {
  const bool PreviousMetaContext = CurrentMetaContext;
  CurrentMetaContext = true;
  auto BlockType = CheckBlockExpression(Expression, Expected, false);
  CurrentMetaContext = PreviousMetaContext;
  if (!BlockType)
    return std::nullopt;
  auto Value = EvaluateConstant(Expression);
  if (!Value || Value->Type == ConstValue::Kind::Void ||
      Value->Type == ConstValue::Kind::Symbol ||
      Value->Type == ConstValue::Kind::Enum) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  auto Literal = std::make_unique<lex::Node>();
  Literal->kind = lex::TokenKind::ast_literal;
  Literal->Loc = Expression.Loc;
  Literal->text = Value->LiteralSpelling();
  if (Value->Type == ConstValue::Kind::Integer &&
      Literal->text.starts_with('-')) {
    const auto Digits = Literal->text.substr(1);
    Literal->kind = lex::TokenKind::ast_unary;
    Literal->text = "-";
    auto Child = std::make_unique<lex::Node>();
    Child->kind = lex::TokenKind::ast_literal;
    Child->Loc = Expression.Loc;
    Child->text = Digits;
    Literal->children.push_back(std::move(Child));
  }
  auto Result = CheckExpression(*Literal, *BlockType);
  if (!Result)
    return std::nullopt;
  ConstantReferences[&Expression] = Literal.get();
  EvaluatedConstants[&Expression] = std::move(Literal);
  return FinishExpression(Expression, *Result, Expected);
}

std::optional<sema::Type>
sema::Sema::CheckBlockExpression(const lex::Node &Expression,
                                 std::optional<Type> Expected, bool) {
  Scopes.emplace_back();
  LocalTypeScopes.emplace_back();
  Type Result{BuiltinType::Void, {}};
  for (std::size_t I = 0; I < Expression.children.size(); ++I) {
    const auto &Child = *Expression.children[I];
    const bool Tail = I + 1 == Expression.children.size() &&
                      lex::IsExpressionNode(Child.kind);
    if (Tail) {
      auto Value = CheckExpression(Child, Expected);
      if (Value)
        Result = *Value;
    } else {
      CheckStatement(Child, CurrentLoopDepth);
    }
  }
  LocalTypeScopes.pop_back();
  Scopes.pop_back();
  if (AlwaysReturns(Expression) && Expected)
    Result = *Expected;
  return FinishExpression(Expression, Result, Expected);
}

std::optional<sema::Type>
sema::Sema::CheckMatchExpression(const lex::Node &Expression,
                                 std::optional<Type> Expected, bool) {
  using K = lex::TokenKind;
  if (Expression.children.size() < 2) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  auto Scrutinee = CheckExpression(*Expression.children.front());
  if (!Scrutinee)
    return std::nullopt;
  if (Scrutinee->IsPointer() || Scrutinee->IsArray() || Scrutinee->IsSlice() ||
      (!Scrutinee->IsEnum() && Scrutinee->Element != BuiltinType::Bool &&
       !IsInteger(Scrutinee->Element))) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  std::unordered_set<std::string> Seen;
  bool Wildcard = false;
  std::optional<Type> Result = Expected;
  for (std::size_t I = 1; I < Expression.children.size(); ++I) {
    const auto &Arm = *Expression.children[I];
    const auto &Pattern = *Arm.children.front();
    if (Wildcard) {
      Error(Pattern, lex::DiagnosticKind::UnreachableMatchArm);
      continue;
    }
    if (Pattern.kind == K::ast_name && Pattern.text == "_") {
      if ((Scrutinee->Element == BuiltinType::Bool && Seen.size() == 2) ||
          (Scrutinee->IsEnum() &&
           Seen.size() == GetEnum(Scrutinee->EnumName)->Variants.size()))
        Error(Pattern, lex::DiagnosticKind::UnreachableMatchArm);
      Wildcard = true;
    } else {
      auto PatternType = CheckExpression(Pattern, *Scrutinee);
      if (!PatternType || *PatternType != *Scrutinee) {
        Error(Pattern, lex::DiagnosticKind::TypeMismatch);
        continue;
      }
      std::optional<std::string> Value;
      const lex::Node *Atom = &Pattern;
      while (Atom->kind == K::ast_group && Atom->children.size() == 1)
        Atom = Atom->children.front().get();
      if (Scrutinee->Element == BuiltinType::Bool &&
          Atom->kind == K::ast_literal &&
          (Atom->text == "true" || Atom->text == "false"))
        Value = Atom->text == "true" ? "1" : "0";
      else if (Scrutinee->IsEnum() && GetEnumVariant(*Atom))
        Value = GetEnumVariant(*Atom)->Value;
      else if (!Scrutinee->IsEnum() && Scrutinee->Element != BuiltinType::Bool)
        Value = EvaluateIntegerConstant(Pattern);
      if (!Value) {
        Error(Pattern, lex::DiagnosticKind::InvalidMatchPattern);
        continue;
      }
      if (!Seen.insert(*Value).second)
        Error(Pattern, lex::DiagnosticKind::UnreachableMatchArm);
      MatchPatternValues[&Pattern] = *Value;
    }
    auto ValueType = CheckExpression(*Arm.children.back(), Result);
    if (ValueType && !Result && !AlwaysReturns(*Arm.children.back()))
      Result = *ValueType;
  }
  if (!Wildcard) {
    bool Complete = false;
    if (Scrutinee->Element == BuiltinType::Bool)
      Complete = Seen.contains("0") && Seen.contains("1");
    else if (Scrutinee->IsEnum()) {
      const auto *Enum = GetEnum(Scrutinee->EnumName);
      Complete = Enum && Enum->Variants.size() == Seen.size();
    }
    if (!Complete)
      Error(Expression, lex::DiagnosticKind::NonExhaustiveMatch);
  }
  return FinishExpression(
      Expression, Result.value_or(Type{BuiltinType::Void, {}}), Expected);
}

std::optional<sema::Type>
sema::Sema::FinishExpression(const lex::Node &Expression, Type Result,
                             std::optional<Type> Expected) {
  Types[&Expression] = Result;
  if (Expected && Expected->IsReadOnlySlice() && Result.IsSlice() &&
      !Result.IsReadOnlySlice() && Expected->Indexed() == Result.Indexed())
    return Result;
  if (Expected && Expected->IsPointer() && Result.IsPointer() &&
      Expected->PointerDepth == 1 && Result.PointerDepth == 1 &&
      Expected->Element == BuiltinType::Class &&
      Result.Element == BuiltinType::Class) {
    const auto *Target = GetClass(Expected->ClassName);
    if (Target && Target->IsInterface && *Expected != Result) {
      std::function<bool(const ClassInfo &)> ExtendsInterface =
          [&](const ClassInfo &Interface) {
            if (Interface.QualifiedName == Target->QualifiedName)
              return true;
            for (const auto &Parent : Interface.Interfaces)
              if (ExtendsInterface(*GetClass(Parent)))
                return true;
            return false;
          };
      const auto *Concrete = GetClass(Result.ClassName);
      if (Concrete && Concrete->IsInterface && ExtendsInterface(*Concrete)) {
        InterfaceConversions[&Expression] = Target->QualifiedName;
        return Result;
      }
      while (Concrete && !Concrete->IsInterface) {
        for (const auto &Interface : Concrete->Interfaces)
          if (ExtendsInterface(*GetClass(Interface))) {
            InterfaceConversions[&Expression] = Target->QualifiedName;
            return Result;
          }
        Concrete =
            Concrete->BaseName.empty() ? nullptr : GetClass(Concrete->BaseName);
      }
    }
    auto *Class = GetClass(Result.ClassName);
    while (Class && !Class->BaseName.empty()) {
      if (Class->BaseName == Expected->ClassName)
        return Result;
      Class = GetClass(Class->BaseName);
    }
  }
  if (Expected && *Expected != Result &&
      !IsCInteropCompatible(*Expected, Result))
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckNameExpression(const lex::Node &Expression,
                                std::optional<Type> Expected, bool) {
  if (Expression.text == "super" && !CurrentClass.empty()) {
    const auto *Class = GetClass(CurrentClass);
    if (!Class->BaseName.empty()) {
      Type Base{BuiltinType::Class, {}};
      Base.ClassName = Class->BaseName;
      Base.AddPointer();
      return FinishExpression(Expression, Base, Expected);
    }
  }
  if (Expression.text == "this" && CurrentConstructor && !CheckingFieldBase &&
      InitializedFields < GetClass(CurrentClass)->UserFieldCount) {
    Error(Expression, lex::DiagnosticKind::UninitializedField);
    return std::nullopt;
  }
  const auto *Result =
      Expression.BoundFieldName ? nullptr : FindName(Expression.text);
  if (!Result && !CurrentClass.empty()) {
    const auto *Class = GetClass(CurrentClass);
    if (Class) {
      for (const ClassInfo *Owner = Class; Owner;
           Owner = Owner->BaseName.empty() ? nullptr
                                           : GetClass(Owner->BaseName))
        for (std::size_t I = 0; I < Owner->StaticFields.size(); ++I)
          if (Owner->StaticFields[I].Name == Expression.text) {
            StaticFieldReferences[&Expression] = {Owner->QualifiedName, I};
            return FinishExpression(Expression, Owner->StaticFields[I].Value,
                                    Expected);
          }
      for (const auto &Constant : Class->Constants)
        if (Constant.Name == Expression.text) {
          ConstantReferences[&Expression] = Constant.Value;
          return FinishExpression(Expression, Constant.ValueType, Expected);
        }
      for (const ClassInfo *Owner = FindName("this") ? Class : nullptr; Owner;
           Owner = Owner->BaseName.empty() ? nullptr
                                           : GetClass(Owner->BaseName))
        for (std::size_t I = Owner == Class ? 0 : Owner->OwnFieldStart;
             I < Owner->UserFieldCount; ++I)
          if (Owner->Fields[I].Name == Expression.text) {
            if (CurrentConstructor && I >= InitializedFields &&
                Owner == Class && InitializingTarget != &Expression) {
              Error(Expression, lex::DiagnosticKind::UninitializedField);
              return std::nullopt;
            }
            FieldReferences[&Expression] = {Owner->QualifiedName, I};
            return FinishExpression(Expression, Owner->Fields[I].Value,
                                    Expected);
          }
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
  if (Root->kind == lex::TokenKind::ast_name && Root->text == "c" &&
      !FindName("c"))
    if (auto Name = QualifiedName(Expression)) {
      const auto Constant = ExternalConstants.find(*Name);
      if (Constant != ExternalConstants.end()) {
        ExternalConstantReferences.emplace(&Expression, Constant->second);
        return FinishExpression(Expression, Constant->second.Value, Expected);
      }
    }
  if (Root->kind == lex::TokenKind::ast_name && !FindName(Root->text)) {
    if (auto Name = QualifiedName(Expression)) {
      const auto Dot = Name->rfind('.');
      if (Dot != std::string::npos) {
        const auto OwnerName = Name->substr(0, Dot);
        const EnumInfo *Enum = GetEnum(OwnerName);
        if (!Enum && !CurrentModule.empty())
          Enum = GetEnum(CurrentModule + "." + OwnerName);
        if (!Enum && OwnerName.find('.') == std::string::npos) {
          const auto Import = Imports.find(CurrentModule);
          if (Import != Imports.end())
            for (const auto &Module : Import->second) {
              const auto *Candidate = GetEnum(Module + "." + OwnerName);
              if (!Candidate || !Candidate->Public)
                continue;
              if (Enum) {
                Error(Expression, lex::DiagnosticKind::AmbiguousName);
                return std::nullopt;
              }
              Enum = Candidate;
            }
        }
        if (Enum) {
          if (MetaRestrictionsReady && !CurrentMetaContext &&
              (MetaModules.contains(Enum->Module) ||
               MetaDeclarations.contains(Enum->Node))) {
            Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
            return std::nullopt;
          }
          const auto Import = Imports.find(CurrentModule);
          if (Enum->Module != CurrentModule &&
              (!Enum->Public || Import == Imports.end() ||
               !Import->second.contains(Enum->Module))) {
            Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
            return std::nullopt;
          }
          for (const auto &Variant : Enum->Variants)
            if (Variant.Name == Expression.text) {
              EnumVariantReferences[&Expression] = Variant;
              Type Result{BuiltinType::Enum, {}};
              Result.EnumName = Enum->QualifiedName;
              Result.BitWidth = GetBitWidth(Enum->Underlying);
              Result.Alignment = std::max(1u, Result.BitWidth / 8);
              return FinishExpression(Expression, Result, Expected);
            }
          Error(Expression, lex::DiagnosticKind::UnknownName);
          return std::nullopt;
        }
        const auto *Owner = GetClass(OwnerName);
        if (!Owner && !CurrentModule.empty())
          Owner = GetClass(CurrentModule + "." + OwnerName);
        if (!Owner && OwnerName.find('.') == std::string::npos) {
          const auto Import = Imports.find(CurrentModule);
          if (Import != Imports.end())
            for (const auto &Module : Import->second) {
              const auto *Candidate = GetClass(Module + "." + OwnerName);
              if (!Candidate || !Candidate->Public)
                continue;
              if (Owner) {
                Error(Expression, lex::DiagnosticKind::AmbiguousName);
                return std::nullopt;
              }
              Owner = Candidate;
            }
        }
        if (Owner) {
          if ((MetaModules.contains(Owner->Module) ||
               MetaDeclarations.contains(Owner->Node)) &&
              !CurrentMetaContext) {
            Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
            return std::nullopt;
          }
          if (!CurrentMetaContext)
            RuntimeDependencies[CurrentModule].insert(Owner->Module);
          for (std::size_t I = 0; I < Owner->StaticFields.size(); ++I) {
            const auto &Field = Owner->StaticFields[I];
            if (Field.Name != Expression.text)
              continue;
            const auto Import = Imports.find(CurrentModule);
            if (Owner->Module != CurrentModule &&
                (!Owner->Public || !Field.Public || Import == Imports.end() ||
                 !Import->second.contains(Owner->Module))) {
              Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
              return std::nullopt;
            }
            StaticFieldReferences[&Expression] = {Owner->QualifiedName, I};
            return FinishExpression(Expression, Field.Value, Expected);
          }
        }
        if (Owner && Owner->IsInterface)
          for (const auto &Constant : Owner->Constants)
            if (Constant.Name == Expression.text) {
              const auto Import = Imports.find(CurrentModule);
              if (Owner->Module != CurrentModule &&
                  (!Owner->Public || !Constant.Public ||
                   Import == Imports.end() ||
                   !Import->second.contains(Owner->Module))) {
                Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
                return std::nullopt;
              }
              ConstantReferences[&Expression] = Constant.Value;
              return FinishExpression(Expression, Constant.ValueType, Expected);
            }
      }
    }
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
  auto Record = Base->IsPointer() ? Base->Pointee() : *Base;
  if (Record.IsRecord()) {
    const auto Fields = ExternalFields.find("c." + Record.CName);
    if (Fields == ExternalFields.end()) {
      Error(Expression, lex::DiagnosticKind::UnsupportedCField);
      return std::nullopt;
    }
    for (const auto &Field : Fields->second)
      if (Field.Name == Expression.text) {
        if (!Field.Addressable) {
          Error(Expression, lex::DiagnosticKind::UnsupportedCField);
          return std::nullopt;
        }
        ExternalFieldReferences.emplace(&Expression, Field);
        return FinishExpression(Expression, Field.Value, Expected);
      }
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }
  const auto *Class = GetClass(*Base);
  if (!Class && Base->IsPointer()) {
    auto Pointee = Base->Pointee();
    Class = GetClass(Pointee);
  }
  if ((Base->IsSlice() || Base->IsArray()) && Expression.text == "len")
    return FinishExpression(Expression, Type{BuiltinType::USize, {}}, Expected);
  if (!Class || Base->PointerDepth > 1 || Base->IsArray()) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  for (const ClassInfo *Owner = Class; Owner;
       Owner = Owner->BaseName.empty() ? nullptr : GetClass(Owner->BaseName))
    for (std::size_t I = Owner == Class ? 0 : Owner->OwnFieldStart;
         I < Owner->UserFieldCount; ++I) {
      const auto &Field = Owner->Fields[I];
      if (Field.Name != Expression.text)
        continue;
      const auto &Receiver = *Expression.children.front();
      if (CurrentConstructor && Owner == Class &&
          Receiver.kind == lex::TokenKind::ast_name &&
          Receiver.text == "this" && I >= InitializedFields &&
          InitializingTarget != &Expression) {
        Error(Expression, lex::DiagnosticKind::UninitializedField);
        return std::nullopt;
      }
      if (Owner->Module != CurrentModule && (!Owner->Public || !Field.Public)) {
        Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
        return std::nullopt;
      }
      FieldReferences[&Expression] = {Owner->QualifiedName, I};
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
    String.AddPointer();
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
  if (Result.IsArray() || Result.IsSlice() || Result.IsPointer() ||
      Result.IsRecord() || Result.IsClass() || Result.IsVoid() ||
      Result.IsResults() || Result.IsFunction()) {
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
  if ((!Base->IsArray() && !Base->IsSlice()) || Index->IsArray() ||
      Index->IsSlice() || Index->IsPointer() || !IsInteger(Index->Element) ||
      GetBitWidth(*Index) > 64) {
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  return FinishExpression(Expression, Base->Indexed(), Expected);
}

std::optional<sema::Type>
sema::Sema::CheckSliceExpression(const lex::Node &Expression,
                                 std::optional<Type> Expected, bool) {
  const auto &BaseNode = *Expression.children.front();
  auto Base = CheckExpression(BaseNode);
  if (!Base || (!Base->IsArray() && !Base->IsSlice()) ||
      (Base->IsArray() && !IsAddressable(BaseNode))) {
    Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return std::nullopt;
  }
  for (std::size_t I = 1; I < Expression.children.size(); ++I) {
    auto Bound = CheckExpression(*Expression.children[I]);
    if (!Bound || Bound->IsPointer() || Bound->IsArray() || Bound->IsSlice() ||
        !IsInteger(Bound->Element) || GetBitWidth(*Bound) > 64) {
      Error(*Expression.children[I], lex::DiagnosticKind::TypeMismatch);
      return std::nullopt;
    }
  }
  auto Result = Base->Indexed();
  Result.AddSlice(Base->IsReadOnlySlice() ||
                  (Expected && Expected->IsReadOnlySlice()));
  return FinishExpression(Expression, Result, Expected);
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
  if (Callee.kind == K::ast_name && Callee.text == "super" &&
      CurrentConstructor && InitializedFields == 0 && !CurrentClass.empty()) {
    const auto *Derived = GetClass(CurrentClass);
    const auto *Base =
        Derived->BaseName.empty() ? nullptr : GetClass(Derived->BaseName);
    if (!Base) {
      Error(Expression, lex::DiagnosticKind::InvalidClass);
      return std::nullopt;
    }
    const auto It = Functions.find(Base->QualifiedName + ".init");
    const auto Count = Expression.children.size() - 1;
    if (Base->Constructor && It == Functions.end()) {
      if (Base->Module != CurrentModule && !IsPublic(*Base->Constructor))
        Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      if (!CheckForwardConstructor(Expression, *Base))
        return std::nullopt;
      BaseConstructorCalls[&Expression] = Base->QualifiedName;
      return FinishExpression(Expression, Type{BuiltinType::Void, {}},
                              Expected);
    }
    if ((It != Functions.end() && It->second.Parameters.size() != Count + 1) ||
        (It == Functions.end() && Count != 0))
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
    if (It != Functions.end()) {
      if (Base->Module != CurrentModule && !It->second.Public)
        Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      for (std::size_t I = 0; I < Count && I + 1 < It->second.Parameters.size();
           ++I) {
        const auto &Argument = *Expression.children[I + 1];
        auto Value = CheckExpression(Argument, It->second.Parameters[I + 1]);
        if (Value && Value->IsClass())
          CheckTransferAccess(*Value, IsClassTemporary(Argument), Argument);
      }
      Callees[&Expression] = It->second.Symbol;
    } else {
      Callees[&Expression] = Base->ConstructorSymbol;
    }
    BaseConstructorCalls[&Expression] = Base->QualifiedName;
    return FinishExpression(Expression, Type{BuiltinType::Void, {}}, Expected);
  }
  const auto &LookupModule = Expression.AnnotationOriginModule.empty()
                                 ? CurrentModule
                                 : Expression.AnnotationOriginModule;
  const auto LocalKey = [&](std::string_view Value) {
    return LookupModule.empty() ? std::string(Value)
                                : LookupModule + "." + std::string(Value);
  };
  const auto Imported = [&](std::string_view Module) {
    const auto It = Imports.find(LookupModule);
    return Module == LookupModule ||
           (It != Imports.end() && It->second.contains(std::string(Module)));
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
  const bool ValueRoot = Root->kind != K::ast_name || IsLocal(Root->text) ||
                         (Root->kind == K::ast_name && Root->text == "super");
  if (ValueRoot && Callee.kind != K::ast_member)
    return CheckIndirectCall(Expression, Expected);
  std::string Key = Name.value_or("");
  if (Name && Name->find('.') == std::string::npos)
    Key = LocalKey(*Name);
  if (Name && Name->find('.') == std::string::npos && !GetClass(Key) &&
      !Functions.contains(Key)) {
    const auto Import = Imports.find(LookupModule);
    if (Import != Imports.end())
      for (const auto &Module : Import->second) {
        const auto *Candidate = GetClass(Module + "." + *Name);
        if (!Candidate || !Candidate->Public)
          continue;
        if (Key != LocalKey(*Name)) {
          Error(Expression, lex::DiagnosticKind::AmbiguousName);
          return std::nullopt;
        }
        Key = Candidate->QualifiedName;
      }
    if (Key != LocalKey(*Name) && Import != Imports.end())
      for (const auto &Module : Import->second) {
        const auto Function = Functions.find(Module + "." + *Name);
        if (Function != Functions.end() && Function->second.Public &&
            Function->second.OwnerClass.empty()) {
          Error(Expression, lex::DiagnosticKind::AmbiguousName);
          return std::nullopt;
        }
      }
  }
  const auto *Class = !ValueRoot ? GetClass(Key) : nullptr;
  if (Class) {
    if ((MetaModules.contains(Class->Module) ||
         MetaDeclarations.contains(Class->Node)) &&
        !CurrentMetaContext) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    if (!CurrentMetaContext)
      RuntimeDependencies[CurrentModule].insert(Class->Module);
    if (Class->IsInterface) {
      Error(Expression, lex::DiagnosticKind::InvalidClass);
      return std::nullopt;
    }
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
    if (Class->Constructor && !Explicit) {
      if (!CheckForwardConstructor(Expression, *Class))
        return std::nullopt;
      Type Result{BuiltinType::Class, {}};
      Result.ClassName = Class->QualifiedName;
      ConstructorCalls[&Expression] = Result.ClassName;
      return FinishExpression(Expression, Result, Expected);
    }
    if (Explicit) {
      if (ArgumentCount + 1 != Function->second.Parameters.size()) {
        Error(Expression, lex::DiagnosticKind::TypeMismatch);
        return std::nullopt;
      }
      for (std::size_t I = 0; I < ArgumentCount; ++I) {
        const auto *Previous = ConstructionContext;
        ConstructionContext = Expression.children[I + 1].get();
        auto Argument = CheckExpression(*Expression.children[I + 1],
                                        Function->second.Parameters[I + 1]);
        ConstructionContext = Previous;
        if (Argument && Argument->IsClass())
          CheckTransferAccess(*Argument,
                              IsClassTemporary(*Expression.children[I + 1]),
                              *Expression.children[I + 1]);
      }
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
  const ClassInfo *InterfaceReceiver = nullptr;
  std::size_t InterfaceMethodIndex = 0;
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
    if (Callee.text == "init" || Callee.text == "deinit" ||
        Callee.text == "copy" || Callee.text == "move") {
      Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
      return std::nullopt;
    }
    if (Class->IsInterface) {
      InterfaceReceiver = Class;
      const auto Method = std::find_if(
          Class->InterfaceMethods.begin(), Class->InterfaceMethods.end(),
          [&](const auto &Candidate) {
            return Candidate.substr(Candidate.rfind('.') + 1) == Callee.text;
          });
      if (Method != Class->InterfaceMethods.end()) {
        InterfaceMethodIndex = Method - Class->InterfaceMethods.begin();
        Key = *Method;
      }
    } else {
      Key = Class->QualifiedName + "." + Callee.text;
      while (!Functions.contains(Key) && !Class->BaseName.empty()) {
        Class = GetClass(Class->BaseName);
        Key = Class->QualifiedName + "." + Callee.text;
      }
    }
    MethodCall = true;
  } else if (!Name || ValueRoot) {
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }

  auto Function = Functions.find(Key);
  if (Function == Functions.end() && Name && !ValueRoot &&
      Name->find('.') != std::string::npos && !LookupModule.empty()) {
    Key = LookupModule + "." + *Name;
    Function = Functions.find(Key);
  }
  if (Function == Functions.end() && Name && !ValueRoot) {
    const auto Dot = Name->rfind('.');
    if (Dot != std::string::npos) {
      const auto OwnerName = Name->substr(0, Dot);
      if (OwnerName.find('.') == std::string::npos) {
        const auto Import = Imports.find(LookupModule);
        if (Import != Imports.end())
          for (const auto &Module : Import->second) {
            const auto *Owner = GetClass(Module + "." + OwnerName);
            if (!Owner || !Owner->Public)
              continue;
            const auto Candidate =
                Functions.find(Owner->QualifiedName + "." + Callee.text);
            if (Candidate == Functions.end())
              continue;
            if (Function != Functions.end()) {
              Error(Expression, lex::DiagnosticKind::AmbiguousName);
              return std::nullopt;
            }
            Function = Candidate;
          }
      }
    }
  }
  if (!MethodCall && Name && Name->find('.') == std::string::npos) {
    if (Function == Functions.end()) {
      const auto Import = Imports.find(LookupModule);
      if (Import != Imports.end())
        for (const auto &Module : Import->second) {
          const auto Candidate = Functions.find(Module + "." + *Name);
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
      auto Method = Functions.find(CurrentClass + "." + *Name);
      const ClassInfo *Owner = GetClass(CurrentClass);
      while (Method == Functions.end() && Owner && !Owner->BaseName.empty()) {
        Owner = GetClass(Owner->BaseName);
        Method = Functions.find(Owner->QualifiedName + "." + *Name);
      }
      if (Method != Functions.end()) {
        if (*Name == "init" || *Name == "deinit" || *Name == "copy" ||
            *Name == "move") {
          Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
          return std::nullopt;
        }
        if (Function != Functions.end()) {
          Error(Expression, lex::DiagnosticKind::AmbiguousName);
          return std::nullopt;
        }
        if (!Method->second.Static && !FindName("this")) {
          Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
          return std::nullopt;
        }
        if (CurrentConstructor &&
            InitializedFields < GetClass(CurrentClass)->UserFieldCount) {
          Error(Expression, lex::DiagnosticKind::UninitializedField);
          return std::nullopt;
        }
        Function = Method;
        MethodCall = !Method->second.Static;
      }
    }
  }
  if (Function == Functions.end()) {
    Error(Expression, lex::DiagnosticKind::UnknownName);
    return std::nullopt;
  }
  const auto &Info = Function->second;
  if ((MetaModules.contains(Info.Module) ||
       (Info.Node && MetaDeclarations.contains(Info.Node)) ||
       (!Info.OwnerClass.empty() &&
        MetaDeclarations.contains(Classes.at(Info.OwnerClass).Node))) &&
      !CurrentMetaContext) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  if (!CurrentMetaContext)
    RuntimeDependencies[CurrentModule].insert(Info.Module);
  if (Key.starts_with("std.memory.init_default__G") &&
      !Info.Parameters.empty()) {
    const auto &Value = Info.Parameters.front();
    const auto *Target =
        Value.IsPointer() ? GetClass(Value.Pointee()) : nullptr;
    if (!Target || !Target->DefaultConstructible) {
      Error(Expression, lex::DiagnosticKind::InvalidClass);
      return std::nullopt;
    }
    if (Target->Module != CurrentModule &&
        (!Target->Public ||
         (Target->Constructor && !IsPublic(*Target->Constructor)))) {
      Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
      return std::nullopt;
    }
  }
  if (Info.Static && MethodCall) {
    Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
    return std::nullopt;
  }
  if (Info.Static)
    MethodCall = false;
  if (Info.Abstract && !InterfaceReceiver) {
    Error(Expression, lex::DiagnosticKind::InvalidClass);
    return std::nullopt;
  }
  if (!Info.OwnerClass.empty() && !Info.Static && !MethodCall) {
    Error(Expression, lex::DiagnosticKind::InvalidLifecycleCall);
    return std::nullopt;
  }
  if (Info.Static && Info.Module != CurrentModule &&
      Info.Module != LookupModule && !GetClass(Info.OwnerClass)->Public) {
    Error(Expression, lex::DiagnosticKind::PrivateDeclaration);
    return std::nullopt;
  }
  if ((!MethodCall && !Imported(Info.Module)) ||
      (Info.Module != CurrentModule && Info.Module != LookupModule &&
       !Info.Public)) {
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
    if (auto Value = [&]() {
          const auto *Previous = ConstructionContext;
          ConstructionContext =
              Expression.children[I + 1 - ParameterOffset].get();
          auto Result =
              CheckExpression(*Expression.children[I + 1 - ParameterOffset],
                              Info.Parameters[I]);
          ConstructionContext = Previous;
          return Result;
        }()) {
      if (Value->IsClass())
        CheckTransferAccess(
            *Value,
            IsClassTemporary(*Expression.children[I + 1 - ParameterOffset]),
            *Expression.children[I + 1 - ParameterOffset]);
      Arguments.push_back(*Value);
    }
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
      ResultPointer.AddPointer();
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
        Pointer.AddPointer();
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
  if (InterfaceReceiver)
    InterfaceCalls[&Expression] = {InterfaceReceiver->QualifiedName,
                                   InterfaceMethodIndex};
  if (MethodCall && (Info.Virtual || Info.Override) &&
      !(Callee.kind == K::ast_member &&
        Callee.children.front()->kind == K::ast_name &&
        Callee.children.front()->text == "super")) {
    for (auto OwnerName = Info.OwnerClass; !OwnerName.empty();) {
      const auto *Owner = GetClass(OwnerName);
      if (const auto Slot = Owner->VirtualSlots.find(Callee.text);
          Slot != Owner->VirtualSlots.end()) {
        VirtualCalls[&Expression] = {OwnerName, Slot->second};
        break;
      }
      OwnerName = Owner->BaseName;
    }
  }
  WarnIfDeprecated(Callee, Info.Node);
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
    if (Result && Operand->kind == lex::TokenKind::ast_index &&
        Types.at(Operand->children.front().get()).IsReadOnlySlice()) {
      Error(Expression, lex::DiagnosticKind::InvalidAssignmentTarget);
      return std::nullopt;
    }
    if (GetFunctionValue(*Operand) || GetConstant(*Operand) ||
        GetExternalConstant(*Operand) || GetEnumVariant(*Operand)) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    if (!Result) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    Result->AddPointer();
    return FinishExpression(Expression, *Result, Expected);
  }
  if (Expression.text == "*") {
    auto Result = CheckExpression(*Expression.children.front());
    if (!Result || !Result->IsPointer()) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    *Result = Result->Pointee();
    if (Result->IsClass()) {
      const auto *Class = GetClass(*Result);
      if (Class && Class->IsInterface) {
        Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
        return std::nullopt;
      }
    }
    if (!Result->IsPointer()) {
      const auto External = ExternalTypes.find("c." + Result->CName);
      if (Result->Element == BuiltinType::CRecord &&
          External != ExternalTypes.end())
        *Result = External->second;
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
    if (Lhs->IsArray() || Lhs->IsSlice() || Lhs->IsClass() || Lhs->IsRecord() ||
        Lhs->IsVoid() || Lhs->IsResults() || Lhs->IsFunction() ||
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

std::optional<sema::Type>
sema::Sema::CheckCastExpression(const lex::Node &Expression,
                                std::optional<Type> Expected, bool) {
  if (Expression.children.size() != 2) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  auto Source = CheckExpression(*Expression.children.front());
  auto Target = CheckType(*Expression.children.back());
  if (!Source || !Target)
    return std::nullopt;
  const bool SourceAddress = Source->IsPointer() || Source->IsFunction();
  const bool TargetAddress = Target->IsPointer() || Target->IsFunction();
  const bool PointerCast = SourceAddress && TargetAddress;
  const bool PointerIntegerCast =
      (SourceAddress && !TargetAddress && !Target->IsArray() &&
       IsInteger(Target->Element)) ||
      (TargetAddress && !SourceAddress && !Source->IsArray() &&
       IsInteger(Source->Element));
  const bool NumericCast =
      !Source->IsPointer() && !Target->IsPointer() && !Source->IsArray() &&
      !Target->IsArray() && !Source->IsSlice() && !Target->IsSlice() &&
      IsNumeric(Source->Element) && IsNumeric(Target->Element) &&
      GetBitWidth(*Source) <= 128 && GetBitWidth(*Target) <= 128;
  const bool EnumCast = Source->IsEnum() && !Target->IsPointer() &&
                        !Target->IsArray() && !Target->IsSlice() &&
                        IsInteger(Target->Element);
  if (*Source != *Target && !PointerCast && !PointerIntegerCast &&
      !NumericCast && !EnumCast) {
    Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
    return std::nullopt;
  }
  return FinishExpression(Expression, *Target, Expected);
}
