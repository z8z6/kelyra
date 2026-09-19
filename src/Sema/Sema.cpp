#include "Sema/Sema.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <sstream>

using namespace kelyra;

namespace {
std::string_view IntegerLimit(sema::BuiltinType Type, bool Negated) {
  using T = sema::BuiltinType;
  switch (Type) {
  case T::I8:
    return Negated ? "128" : "127";
  case T::I16:
    return Negated ? "32768" : "32767";
  case T::I32:
    return Negated ? "2147483648" : "2147483647";
  case T::I64:
    return Negated ? "9223372036854775808" : "9223372036854775807";
  case T::I128:
    return Negated ? "170141183460469231731687303715884105728"
                   : "170141183460469231731687303715884105727";
  case T::U8:
    return "255";
  case T::U16:
    return "65535";
  case T::U32:
    return "4294967295";
  case T::U64:
    return "18446744073709551615";
  case T::U128:
    return "340282366920938463463374607431768211455";
  case T::Char:
    return "1114111";
  default:
    return {};
  }
}

bool FitsInteger(std::string_view Text, const sema::Type &Type, bool Negated) {
  if (Negated && !sema::IsSignedInteger(Type.Element))
    return false;
  auto Limit = IntegerLimit(Type.Element, Negated);
  if (Limit.empty() && sema::IsInteger(Type.Element)) {
    using T = sema::BuiltinType;
    const auto Width = sema::GetBitWidth(Type);
    const auto Normalized = sema::IsSignedInteger(Type.Element)
                                ? (Width == 8    ? T::I8
                                   : Width == 16 ? T::I16
                                   : Width == 32 ? T::I32
                                   : Width == 64 ? T::I64
                                                 : T::I128)
                                : (Width == 8    ? T::U8
                                   : Width == 16 ? T::U16
                                   : Width == 32 ? T::U32
                                   : Width == 64 ? T::U64
                                                 : T::U128);
    Limit = IntegerLimit(Normalized, Negated);
  }
  const auto First = Text.find_first_not_of('0');
  Text = First == std::string_view::npos ? "0" : Text.substr(First);
  if (Text.size() != Limit.size())
    return Text.size() < Limit.size();
  if (Text > Limit)
    return false;
  if (Type.Element == sema::BuiltinType::Char) {
    std::uint32_t Value;
    std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    return Value < 0xD800 || Value > 0xDFFF;
  }
  return true;
}

bool IsScalarNumeric(const sema::Type &Type) {
  return !Type.IsArray() && !Type.IsPointer() && sema::IsNumeric(Type.Element);
}

bool IsTypeNode(lex::TokenKind Kind) {
  using K = lex::TokenKind;
  return Kind == K::ast_type || Kind == K::ast_pointer_type ||
         Kind == K::ast_array_type;
}

std::string CSpelling(const sema::Type &Type) {
  if (!Type.CSpelling.empty())
    return Type.CSpelling;
  using T = sema::BuiltinType;
  switch (Type.Element) {
  case T::I8:
    return "signed char";
  case T::I16:
    return "short";
  case T::I32:
    return "int";
  case T::I64:
    return "long long";
  case T::U8:
    return "unsigned char";
  case T::U16:
    return "unsigned short";
  case T::U32:
    return "unsigned int";
  case T::U64:
    return "unsigned long long";
  case T::F32:
    return "float";
  case T::F64:
    return "double";
  case T::Bool:
    return "_Bool";
  case T::Char:
    return "unsigned int";
  default:
    return std::string(GetBuiltinTypeInfo(Type.Element).Name).substr(2);
  }
}

std::string ModuleName(const lex::Node &Module) {
  for (const auto &Child : Module.children)
    if (Child->kind == lex::TokenKind::ast_module_decl)
      return Child->text;
  return {};
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

std::string Mangle(std::string_view Module, std::string_view Name,
                   bool IsEntrypoint) {
  if (Module.empty() || IsEntrypoint)
    return std::string(Name);
  std::ostringstream Result;
  Result << "_K";
  std::size_t Start = 0;
  while (Start < Module.size()) {
    const auto End = Module.find('.', Start);
    const auto Part = Module.substr(Start, End == std::string_view::npos
                                               ? Module.size() - Start
                                               : End - Start);
    Result << Part.size() << Part;
    if (End == std::string_view::npos)
      break;
    Start = End + 1;
  }
  Result << 'F' << Name.size() << Name;
  return Result.str();
}
} // namespace

void sema::Sema::Error(const lex::Node &Node, lex::DiagnosticKind Kind) {
  Diagnostics.push_back({Kind, Node.Loc});
}

const sema::Type *sema::Sema::FindName(std::string_view Name) const {
  for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope) {
    const auto It = Scope->find(std::string(Name));
    if (It != Scope->end())
      return &It->second;
  }
  return nullptr;
}

std::optional<sema::Type> sema::Sema::CheckType(const lex::Node &Node) {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_type) {
    const auto Element = ParseBuiltinType(Node.text);
    const auto External = ExternalTypes.find(Node.text);
    if (!Element && External == ExternalTypes.end()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    Type Result = Element ? Type{*Element, {}} : External->second;
    Types[&Node] = Result;
    return Result;
  }
  if (Node.kind == K::ast_pointer_type && Node.children.size() == 1) {
    auto Result = CheckType(*Node.children.front());
    if (!Result || Result->IsArray()) {
      Error(Node, lex::DiagnosticKind::UnsupportedType);
      return std::nullopt;
    }
    ++Result->PointerDepth;
    Result->BitWidth = sizeof(void *) * 8;
    Result->Alignment = alignof(void *);
    Result->CSpelling = CSpelling(*Result) + " *";
    Types[&Node] = *Result;
    return Result;
  }
  if (Node.kind != K::ast_array_type || Node.children.size() != 1) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  auto Result = CheckType(*Node.children.front());
  std::uint64_t Length = 0;
  const auto Parsed = std::from_chars(
      Node.text.data(), Node.text.data() + Node.text.size(), Length);
  if (!Result || Parsed.ec != std::errc() || Length == 0) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  if (!Result->IsRecord() && GetBitWidth(*Result) > 128) {
    Error(Node, lex::DiagnosticKind::UnsupportedType);
    return std::nullopt;
  }
  Result->Dimensions.push_back(Length);
  Types[&Node] = *Result;
  return Result;
}

std::optional<sema::Type>
sema::Sema::CheckExpression(const lex::Node &Expression,
                            std::optional<Type> Expected, bool Negated) {
  using K = lex::TokenKind;
  auto Finish = [&](Type Result) -> std::optional<Type> {
    Types[&Expression] = Result;
    if (Expected && *Expected != Result &&
        !IsCInteropCompatible(*Expected, Result))
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
    return Result;
  };

  if (Expression.kind == K::ast_name) {
    const auto *Result = FindName(Expression.text);
    if (!Result) {
      Error(Expression, lex::DiagnosticKind::UnknownName);
      return std::nullopt;
    }
    return Finish(*Result);
  }
  if (Expression.kind == K::ast_literal) {
    if (Expression.text == "true" || Expression.text == "false")
      return Finish({BuiltinType::Bool, {}});
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
      return Finish(Expected.value_or(String));
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
               !FitsInteger(Expression.text, Result, Negated)) {
      Error(Expression, lex::DiagnosticKind::InvalidIntegerLiteral);
      return std::nullopt;
    }
    Types[&Expression] = Result;
    return Result;
  }
  if (Expression.kind == K::ast_group && Expression.children.size() == 1) {
    auto Result = CheckExpression(*Expression.children.front(), Expected);
    if (Result)
      Types[&Expression] = *Result;
    return Result;
  }
  if (Expression.kind == K::ast_index && Expression.children.size() == 2) {
    auto Base = CheckExpression(*Expression.children[0]);
    auto Index = CheckExpression(*Expression.children[1]);
    if (!Base || !Index)
      return std::nullopt;
    if (!Base->IsArray() || Index->IsArray() || !IsInteger(Index->Element)) {
      Error(Expression, lex::DiagnosticKind::TypeMismatch);
      return std::nullopt;
    }
    return Finish(Base->Indexed());
  }
  if (Expression.kind == K::ast_call && !Expression.children.empty()) {
    auto Name = QualifiedName(*Expression.children.front());
    if (!Name) {
      Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
      return std::nullopt;
    }
    std::string Key = *Name;
    if (Name->find('.') == std::string::npos && !CurrentModule.empty())
      Key = CurrentModule + "." + *Name;
    const auto Function = Functions.find(Key);
    if (Function == Functions.end()) {
      Error(Expression, lex::DiagnosticKind::UnknownName);
      return std::nullopt;
    }
    const auto &Info = Function->second;
    if (Info.Module != CurrentModule) {
      const auto Import = Imports.find(CurrentModule);
      if (Import == Imports.end() || !Import->second.contains(Info.Module)) {
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
        Source << CSpelling(Info.Return);
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
          Source << CSpelling(Arguments[I]) << " arg" << I;
          Wrapper.Parameters.push_back(Arguments[I]);
        }
      }
      Source << ") { ";
      if (Wrapper.ReturnByAddress)
        Source << "*(" << CSpelling(Info.Return) << " *)result = ";
      else
        Source << "return ";
      Source << Info.External->Name << '(';
      for (std::size_t I = 0; I < Arguments.size(); ++I) {
        if (I)
          Source << ", ";
        if (Arguments[I].IsRecord())
          Source << "*(" << CSpelling(Arguments[I]) << " *)arg" << I;
        else
          Source << "arg" << I;
      }
      Source << "); }\n";
      Wrapper.Source = Source.str();
      CWrapperCalls[&Expression] = CWrappers.size();
      CWrappers.push_back(std::move(Wrapper));
    }
    Callees[&Expression] = Info.Symbol;
    return Finish(Info.Return);
  }
  if (Expression.kind == K::ast_unary && Expression.children.size() == 1) {
    if (Expression.text == "!") {
      const Type Bool{BuiltinType::Bool, {}};
      auto Operand = CheckExpression(*Expression.children.front(), Bool);
      if (!Operand)
        return std::nullopt;
      return Finish(Bool);
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
  if (Expression.kind == K::ast_binary && Expression.children.size() == 2) {
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
      return Finish(Bool);
    if (Comparison) {
      if (Lhs->IsArray() || (Ordered && !IsNumeric(Lhs->Element))) {
        Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
        return std::nullopt;
      }
      return Finish(Bool);
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
  Error(Expression, lex::DiagnosticKind::UnsupportedExpression);
  return std::nullopt;
}

void sema::Sema::CheckStatement(const lex::Node &Statement,
                                unsigned LoopDepth) {
  using K = lex::TokenKind;
  if (Statement.kind == K::ast_block) {
    CheckBlock(Statement, LoopDepth);
    return;
  }
  if (Statement.kind == K::ast_let) {
    const lex::Node *Name = Statement.children.front().get();
    const lex::Node *TypeNode = nullptr;
    const lex::Node *Initializer = nullptr;
    if (Statement.children.size() > 1) {
      const auto &Second = Statement.children[1];
      if (IsTypeNode(Second->kind)) {
        TypeNode = Second.get();
        if (Statement.children.size() == 3)
          Initializer = Statement.children[2].get();
      } else {
        Initializer = Second.get();
      }
    }
    auto Declared = TypeNode ? CheckType(*TypeNode) : std::nullopt;
    auto Initial = Initializer ? CheckExpression(*Initializer, Declared)
                               : std::optional<Type>();
    const auto Result = Declared ? Declared : Initial;
    if (Result && !Result->IsRecord() && GetBitWidth(*Result) > 128) {
      Error(Statement, lex::DiagnosticKind::UnsupportedType);
      return;
    }
    if (Name && Result) {
      Scopes.back()[Name->text] = *Result;
      Types[Name] = *Result;
      Types[&Statement] = *Result;
    }
    return;
  }
  if (Statement.kind == K::ast_assign && Statement.children.size() == 2) {
    auto Target = CheckExpression(*Statement.children[0]);
    if (Target && !Target->IsRecord() && GetBitWidth(*Target) > 128)
      Error(Statement, lex::DiagnosticKind::UnsupportedType);
    else if (Target)
      CheckExpression(*Statement.children[1], Target);
    return;
  }
  if (Statement.kind == K::ast_expr_stmt && Statement.children.size() == 1) {
    CheckExpression(*Statement.children.front());
    return;
  }
  if (Statement.kind == K::ast_return) {
    if (!ReturnType || Statement.children.size() != 1) {
      Error(Statement, lex::DiagnosticKind::MissingReturn);
      return;
    }
    CheckExpression(*Statement.children.front(), ReturnType);
    return;
  }
  if (Statement.kind == K::ast_if) {
    CheckExpression(*Statement.children[0], Type{BuiltinType::Bool, {}});
    CheckBlock(*Statement.children[1], LoopDepth);
    if (Statement.children.size() == 3) {
      if (Statement.children[2]->kind == K::ast_if)
        CheckStatement(*Statement.children[2], LoopDepth);
      else
        CheckBlock(*Statement.children[2], LoopDepth);
    }
    return;
  }
  if (Statement.kind == K::ast_while) {
    CheckExpression(*Statement.children[0], Type{BuiltinType::Bool, {}});
    CheckBlock(*Statement.children[1], LoopDepth + 1);
    return;
  }
  if (Statement.kind == K::ast_break || Statement.kind == K::ast_continue) {
    if (LoopDepth == 0)
      Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
    return;
  }
  Error(Statement, lex::DiagnosticKind::UnsupportedStatement);
}

void sema::Sema::CheckBlock(const lex::Node &Block, unsigned LoopDepth) {
  Scopes.emplace_back();
  for (const auto &Statement : Block.children)
    CheckStatement(*Statement, LoopDepth);
  Scopes.pop_back();
}

bool sema::Sema::AlwaysReturns(const lex::Node &Node) const {
  using K = lex::TokenKind;
  if (Node.kind == K::ast_return)
    return true;
  if (Node.kind == K::ast_block) {
    for (const auto &Child : Node.children)
      if (AlwaysReturns(*Child))
        return true;
    return false;
  }
  return Node.kind == K::ast_if && Node.children.size() == 3 &&
         AlwaysReturns(*Node.children[1]) && AlwaysReturns(*Node.children[2]);
}

void sema::Sema::CheckFunction(const lex::Node &Function) {
  using K = lex::TokenKind;
  Scopes.clear();
  Scopes.emplace_back();
  const lex::Node *ReturnTypeNode = nullptr;
  const lex::Node *Body = nullptr;
  for (const auto &Child : Function.children) {
    if (Child->kind == K::ast_parameter) {
      if (Child->children.size() != 1)
        continue;
      auto ParameterType = CheckType(*Child->children.front());
      if (!ParameterType)
        continue;
      Types[Child.get()] = *ParameterType;
      if (!Scopes.back().emplace(Child->text, *ParameterType).second)
        Error(*Child, lex::DiagnosticKind::DuplicateParameter);
    } else if (IsTypeNode(Child->kind)) {
      ReturnTypeNode = Child.get();
    } else if (Child->kind == K::ast_block) {
      Body = Child.get();
    }
  }
  ReturnType = ReturnTypeNode ? CheckType(*ReturnTypeNode) : std::nullopt;
  if (!ReturnTypeNode)
    Error(Function, lex::DiagnosticKind::UnsupportedType);
  if (!Body || !AlwaysReturns(*Body)) {
    Error(Body ? *Body : Function, lex::DiagnosticKind::MissingReturn);
    return;
  }
  CheckBlock(*Body);
}

bool sema::Sema::Check(const lex::Node &Module) {
  return CheckModules({{&Module, true}});
}

bool sema::Sema::IsPublic(const lex::Node &Node) const {
  using K = lex::TokenKind;
  for (const auto &Child : Node.children)
    if (Child->kind == K::ast_public)
      return true;
  return false;
}

const sema::CWrapper *sema::Sema::GetCWrapper(const lex::Node &Node) const {
  const auto It = CWrapperCalls.find(&Node);
  return It == CWrapperCalls.end() ? nullptr : &CWrappers[It->second];
}

bool sema::Sema::CheckModules(
    const std::vector<ModuleInput> &Modules,
    const std::vector<ExternalFunction> &ExternalDeclarations,
    const std::vector<ExternalType> &ExternalTypeDeclarations) {
  using K = lex::TokenKind;
  Diagnostics.clear();
  Types.clear();
  Functions.clear();
  Symbols.clear();
  Callees.clear();
  CWrapperCalls.clear();
  CWrappers.clear();
  Imports.clear();
  ExternalTypes.clear();
  ExternalFunctions = ExternalDeclarations;
  for (const auto &External : ExternalTypeDeclarations)
    ExternalTypes.emplace(External.Name, External.Value);
  std::unordered_map<std::string, const lex::Node *> ModuleTable;

  for (const auto &Input : Modules) {
    const auto &Module = *Input.Ast;
    if (Module.kind != K::ast_module) {
      Error(Module, lex::DiagnosticKind::UnsupportedDeclaration);
      continue;
    }
    const auto Name = ModuleName(Module);
    if (!ModuleTable.emplace(Name, &Module).second)
      Error(Module, lex::DiagnosticKind::DuplicateModule);
  }

  for (const auto &External : ExternalFunctions) {
    FunctionInfo Info;
    Info.Module = "c";
    Info.Symbol = External.Name;
    Info.Parameters = External.Parameters;
    Info.Return = External.Return;
    Info.Public = true;
    Info.Variadic = External.Variadic;
    Info.External = &External;
    if (!Functions.emplace("c." + External.Name, std::move(Info)).second &&
        !Modules.empty())
      Diagnostics.push_back(
          {lex::DiagnosticKind::DuplicateFunction, Modules.front().Ast->Loc});
  }

  for (const auto &Input : Modules) {
    const auto &Module = *Input.Ast;
    const auto Name = ModuleName(Module);
    for (const auto &Child : Module.children) {
      if (Child->kind == K::ast_module_decl)
        continue;
      if (Child->kind == K::ast_import) {
        Imports[Name].insert(Child->text);
        if (Child->text != "c" && !ModuleTable.contains(Child->text))
          Error(*Child, lex::DiagnosticKind::UnknownModule);
        continue;
      }
      if (Child->kind != K::ast_function) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      FunctionInfo Info;
      Info.Node = Child.get();
      Info.Module = Name;
      Info.Public = IsPublic(*Child);
      Info.Symbol =
          Mangle(Name, Child->text, Input.IsEntry && Child->text == "main");
      for (const auto &Part : Child->children) {
        if (Part->kind == K::ast_parameter && Part->children.size() == 1) {
          if (auto Parameter = CheckType(*Part->children.front())) {
            Info.Parameters.push_back(*Parameter);
            Types[Part.get()] = *Parameter;
          }
        } else if (IsTypeNode(Part->kind)) {
          if (auto Return = CheckType(*Part))
            Info.Return = *Return;
        }
      }
      const auto Key = Name.empty() ? Child->text : Name + "." + Child->text;
      if (!Functions.emplace(Key, Info).second)
        Error(*Child, lex::DiagnosticKind::DuplicateFunction);
      Symbols[Child.get()] = std::move(Info.Symbol);
    }
  }

  for (const auto &Input : Modules) {
    CurrentModule = ModuleName(*Input.Ast);
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_function)
        continue;
      CheckFunction(*Child);
    }
  }
  return Diagnostics.empty();
}

bool sema::Sema::CheckEntrypoint(const lex::Node &Module) {
  using K = lex::TokenKind;
  for (const auto &Function : Module.children) {
    if (Function->kind != K::ast_function || Function->text != "main")
      continue;
    unsigned Parameters = 0;
    const lex::Node *ReturnTypeNode = nullptr;
    for (const auto &Child : Function->children) {
      if (Child->kind == K::ast_parameter)
        ++Parameters;
      else if (IsTypeNode(Child->kind))
        ReturnTypeNode = Child.get();
    }
    const Type Expected{BuiltinType::I32, {}};
    if (Parameters != 0 || !ReturnTypeNode ||
        GetType(*ReturnTypeNode) != Expected)
      Error(*Function, lex::DiagnosticKind::InvalidEntrypoint);
    return Diagnostics.empty();
  }
  Error(Module, lex::DiagnosticKind::MissingEntrypoint);
  return false;
}
