#pragma once

#include "Lexer/Token.h"
#include "Sema/Type.h"

#include <charconv>
#include <cstdint>
#include <string>

namespace kelyra::sema::detail {
inline bool IsTypeNode(lex::TokenKind Kind) {
  using K = lex::TokenKind;
  return Kind == K::ast_type || Kind == K::ast_pointer_type ||
         Kind == K::ast_array_type;
}

inline std::string_view IntegerLimit(BuiltinType Type, bool Negated) {
  using T = BuiltinType;
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

inline bool FitsInteger(std::string_view Text, const Type &Type, bool Negated) {
  if (Negated && !IsSignedInteger(Type.Element))
    return false;
  auto Limit = IntegerLimit(Type.Element, Negated);
  if (Limit.empty() && IsInteger(Type.Element)) {
    using T = BuiltinType;
    const auto Width = GetBitWidth(Type);
    const auto Normalized = IsSignedInteger(Type.Element)
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
  if (Type.Element == BuiltinType::Char) {
    std::uint32_t Value;
    std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    return Value < 0xD800 || Value > 0xDFFF;
  }
  return true;
}

inline std::string CSpelling(const Type &Type) {
  if (!Type.CSpelling.empty())
    return Type.CSpelling;
  using T = BuiltinType;
  switch (Type.Element) {
  case T::I8:
    return "signed char";
  case T::I16:
    return "short";
  case T::I32:
    return "int";
  case T::I64:
    return "long long";
  case T::ISize:
    return "ptrdiff_t";
  case T::U8:
    return "unsigned char";
  case T::U16:
    return "unsigned short";
  case T::U32:
    return "unsigned int";
  case T::U64:
    return "unsigned long long";
  case T::USize:
    return "size_t";
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
} // namespace kelyra::sema::detail
