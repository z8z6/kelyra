#include "Front/Sema/Type.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <memory>

using namespace kelyra;

sema::TargetLayout::TargetLayout(std::string Target) {
  clang::TargetOptions Options;
  Options.Triple = Target.empty() ? llvm::sys::getDefaultTargetTriple() : std::move(Target);
  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> IDs(new clang::DiagnosticIDs);
  clang::DiagnosticOptions DiagnosticOptions;
  clang::IgnoringDiagConsumer Consumer;
  clang::DiagnosticsEngine Diagnostics(IDs, DiagnosticOptions, &Consumer, false);
  std::unique_ptr<clang::TargetInfo> Info(
      clang::TargetInfo::CreateTargetInfo(Diagnostics, Options));
  if (!Info)
    return;
  PointerWidth = Info->getPointerWidth(clang::LangAS::Default);
  PointerAlignment = Info->getPointerAlign(clang::LangAS::Default) / 8;
  using T = BuiltinType;
  const auto Set = [&](T Type, unsigned Width, unsigned Alignment) {
    Widths[static_cast<std::size_t>(Type)] = Width;
    Alignments[static_cast<std::size_t>(Type)] = std::max(1u, Alignment / 8);
  };
  for (std::size_t I = 0; I < BuiltinTypeInfos.size(); ++I) {
    const auto &Type = BuiltinTypeInfos[I];
    unsigned Alignment = std::min(128u, std::max(8u, Type.BitWidth));
    if (Type.Class == TypeClass::SignedInteger || Type.Class == TypeClass::UnsignedInteger) {
      const auto Integer =
          Info->getIntTypeByWidth(Type.BitWidth, Type.Class == TypeClass::SignedInteger);
      if (Integer != clang::TargetInfo::NoInt)
        Alignment = Info->getTypeAlign(Integer);
      else if (Type.BitWidth == 128)
        Alignment = Info->getInt128Align();
    }
    Set(static_cast<T>(I), Type.BitWidth, Alignment);
  }
  Set(T::ISize, PointerWidth, PointerAlignment * 8);
  Set(T::USize, PointerWidth, PointerAlignment * 8);
  Set(T::Function, PointerWidth, PointerAlignment * 8);
  Set(T::CChar, Info->getCharWidth(), Info->getCharAlign());
  Set(T::CSChar, Info->getCharWidth(), Info->getCharAlign());
  Set(T::CUChar, Info->getCharWidth(), Info->getCharAlign());
  Set(T::CShort, Info->getShortWidth(), Info->getShortAlign());
  Set(T::CInt, Info->getIntWidth(), Info->getIntAlign());
  Set(T::CUInt, Info->getIntWidth(), Info->getIntAlign());
  Set(T::CLong, Info->getLongWidth(), Info->getLongAlign());
  Set(T::CLongLong, Info->getLongLongWidth(), Info->getLongLongAlign());
  Set(T::CSize, Info->getTypeWidth(Info->getSizeType()), Info->getTypeAlign(Info->getSizeType()));
  Set(T::CPtrdiff,
      Info->getTypeWidth(Info->getPtrDiffType(clang::LangAS::Default)),
      Info->getTypeAlign(Info->getPtrDiffType(clang::LangAS::Default)));
  Set(T::CBool, Info->getBoolWidth(), Info->getBoolAlign());
  Set(T::CFloat, Info->getFloatWidth(), Info->getFloatAlign());
  Set(T::CDouble, Info->getDoubleWidth(), Info->getDoubleAlign());
  Set(T::CWChar, Info->getWCharWidth(), Info->getWCharAlign());
  Set(T::F32, 32, Info->getFloatAlign());
  Set(T::F64, 64, Info->getDoubleAlign());
  Set(T::F128, 128, Info->getFloat128Align());
}

unsigned sema::TargetLayout::GetBitWidth(const Type &Type) const {
  if (Type.IsPointer())
    return PointerWidth;
  if (Type.IsSlice())
    return PointerWidth * 2;
  if (Type.IsArray())
    return GetBitWidth(Type.Indexed());
  return Type.BitWidth ? Type.BitWidth : Widths[static_cast<std::size_t>(Type.Element)];
}

unsigned sema::TargetLayout::GetAlignment(const Type &Type) const {
  if (Type.IsPointer() || Type.IsSlice())
    return PointerAlignment;
  if (Type.IsArray())
    return GetAlignment(Type.Indexed());
  return Type.Alignment ? Type.Alignment : Alignments[static_cast<std::size_t>(Type.Element)];
}

bool sema::TargetLayout::IsCInteropCompatible(const Type &Left, const Type &Right) const {
  const auto IsCType = [](BuiltinType Value) {
    return Value >= BuiltinType::CChar && Value < BuiltinType::CRecord;
  };
  if (Left.IsArray() || Right.IsArray() || Left.IsSlice() || Right.IsSlice())
    return false;
  if (Left.IsPointer() || Right.IsPointer()) {
    if (!Left.IsPointer() || !Right.IsPointer() || Left.PointerDepth != Right.PointerDepth)
      return false;
    if (Left.Pointee().IsArray() || Right.Pointee().IsArray())
      return false;
    if (Left.PointerDepth > 1) {
      return IsCInteropCompatible(Left.Pointee(), Right.Pointee());
    }
    // A Kelyra pointer to a scalar may cross the C boundary when the pointee
    // matches the C element in signedness and width.
    return IsCType(Left.Element) != IsCType(Right.Element) && IsNumeric(Left.Element) &&
           IsNumeric(Right.Element) &&
           GetBuiltinTypeInfo(Left.Element).Class == GetBuiltinTypeInfo(Right.Element).Class &&
           GetBitWidth(Left.Pointee()) == GetBitWidth(Right.Pointee());
  }
  return IsCType(Left.Element) != IsCType(Right.Element) && IsNumeric(Left.Element) &&
         IsNumeric(Right.Element) &&
         GetBuiltinTypeInfo(Left.Element).Class == GetBuiltinTypeInfo(Right.Element).Class &&
         GetBitWidth(Left) == GetBitWidth(Right);
}
