#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kelyra::sema {
enum class TypeClass {
  SignedInteger,
  UnsignedInteger,
  Float,
  Bool,
  Char,
  Aggregate
};

enum class BuiltinType {
  I8,
  I16,
  I32,
  I64,
  I128,
  U8,
  U16,
  U32,
  U64,
  U128,
  F32,
  F64,
  F128,
  F256,
  F512,
  Bool,
  Char,
  CChar,
  CSChar,
  CUChar,
  CShort,
  CInt,
  CUInt,
  CLong,
  CLongLong,
  CSize,
  CPtrdiff,
  CBool,
  CWChar,
  CRecord,
  Count,
};

struct Type {
  BuiltinType Element;
  std::vector<std::uint64_t> Dimensions;
  unsigned BitWidth = 0;
  unsigned Alignment = 0;
  unsigned PointerDepth = 0;
  std::string CName;
  std::string CSpelling;

  bool IsArray() const { return !Dimensions.empty(); }
  bool IsPointer() const { return PointerDepth != 0; }
  bool IsRecord() const {
    return Element == BuiltinType::CRecord && !IsPointer();
  }

  Type Indexed() const {
    Type Result = *this;
    Result.Dimensions.erase(Result.Dimensions.begin());
    return Result;
  }

  bool operator==(const Type &Other) const {
    return Element == Other.Element && Dimensions == Other.Dimensions &&
           PointerDepth == Other.PointerDepth && CName == Other.CName;
  }
};

struct BuiltinTypeInfo {
  std::string_view Name;
  TypeClass Class;
  unsigned BitWidth;
};

inline constexpr std::array BuiltinTypeInfos = {
    BuiltinTypeInfo{"i8", TypeClass::SignedInteger, 8},
    BuiltinTypeInfo{"i16", TypeClass::SignedInteger, 16},
    BuiltinTypeInfo{"i32", TypeClass::SignedInteger, 32},
    BuiltinTypeInfo{"i64", TypeClass::SignedInteger, 64},
    BuiltinTypeInfo{"i128", TypeClass::SignedInteger, 128},
    BuiltinTypeInfo{"u8", TypeClass::UnsignedInteger, 8},
    BuiltinTypeInfo{"u16", TypeClass::UnsignedInteger, 16},
    BuiltinTypeInfo{"u32", TypeClass::UnsignedInteger, 32},
    BuiltinTypeInfo{"u64", TypeClass::UnsignedInteger, 64},
    BuiltinTypeInfo{"u128", TypeClass::UnsignedInteger, 128},
    BuiltinTypeInfo{"f32", TypeClass::Float, 32},
    BuiltinTypeInfo{"f64", TypeClass::Float, 64},
    BuiltinTypeInfo{"f128", TypeClass::Float, 128},
    BuiltinTypeInfo{"f256", TypeClass::Float, 256},
    BuiltinTypeInfo{"f512", TypeClass::Float, 512},
    BuiltinTypeInfo{"bool", TypeClass::Bool, 1},
    BuiltinTypeInfo{"char", TypeClass::Char, 32},
    BuiltinTypeInfo{"c.char", TypeClass::SignedInteger, 8},
    BuiltinTypeInfo{"c.schar", TypeClass::SignedInteger, 8},
    BuiltinTypeInfo{"c.uchar", TypeClass::UnsignedInteger, 8},
    BuiltinTypeInfo{"c.short", TypeClass::SignedInteger, 16},
    BuiltinTypeInfo{"c.int", TypeClass::SignedInteger, 32},
    BuiltinTypeInfo{"c.uint", TypeClass::UnsignedInteger, 32},
    BuiltinTypeInfo{"c.long", TypeClass::SignedInteger, sizeof(long) * 8},
    BuiltinTypeInfo{"c.longlong", TypeClass::SignedInteger, 64},
    BuiltinTypeInfo{"c.size", TypeClass::UnsignedInteger,
                    sizeof(std::size_t) * 8},
    BuiltinTypeInfo{"c.ptrdiff", TypeClass::SignedInteger,
                    sizeof(std::ptrdiff_t) * 8},
    BuiltinTypeInfo{"c.bool", TypeClass::Bool, 8},
    BuiltinTypeInfo{"c.wchar", TypeClass::SignedInteger, sizeof(wchar_t) * 8},
    BuiltinTypeInfo{"", TypeClass::Aggregate, 0},
};
static_assert(BuiltinTypeInfos.size() ==
              static_cast<std::size_t>(BuiltinType::Count));

inline const BuiltinTypeInfo &GetBuiltinTypeInfo(BuiltinType Type) {
  return BuiltinTypeInfos[static_cast<std::size_t>(Type)];
}

inline unsigned GetBitWidth(const Type &Type) {
  return Type.BitWidth ? Type.BitWidth
                       : GetBuiltinTypeInfo(Type.Element).BitWidth;
}

inline std::optional<BuiltinType> ParseBuiltinType(std::string_view Name) {
  for (std::size_t I = 0; I < BuiltinTypeInfos.size(); ++I)
    if (BuiltinTypeInfos[I].Name == Name)
      return static_cast<BuiltinType>(I);
  return std::nullopt;
}

inline bool IsInteger(BuiltinType Type) {
  const auto Class = GetBuiltinTypeInfo(Type).Class;
  return Class == TypeClass::SignedInteger ||
         Class == TypeClass::UnsignedInteger;
}

inline bool IsSignedInteger(BuiltinType Type) {
  return GetBuiltinTypeInfo(Type).Class == TypeClass::SignedInteger;
}

inline bool IsFloat(BuiltinType Type) {
  return GetBuiltinTypeInfo(Type).Class == TypeClass::Float;
}

inline bool IsNumeric(BuiltinType Type) {
  return IsInteger(Type) || IsFloat(Type);
}

inline bool IsCInteropCompatible(const Type &Left, const Type &Right) {
  const auto IsCType = [](BuiltinType Value) {
    return Value >= BuiltinType::CChar && Value < BuiltinType::CRecord;
  };
  return !Left.IsArray() && !Right.IsArray() && !Left.IsPointer() &&
         !Right.IsPointer() &&
         IsCType(Left.Element) != IsCType(Right.Element) &&
         IsNumeric(Left.Element) && IsNumeric(Right.Element) &&
         GetBuiltinTypeInfo(Left.Element).Class ==
             GetBuiltinTypeInfo(Right.Element).Class &&
         GetBitWidth(Left) == GetBitWidth(Right);
}
} // namespace kelyra::sema
