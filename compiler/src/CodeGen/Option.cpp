#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

using namespace kelyra;

bool codegen::IRGen::EmitOptionIntrinsic(const lex::Node &Function, mlir::func::FuncOp Func,
                                         bool DeclarationOnly) {
  const auto Id = Analysis.GetReflection().GetId(Function);
  if (!Id)
    return false;
  const auto &Name = Analysis.GetReflection().Get(*Id).QualifiedName;
  const bool Size = Name.starts_with("std.util.iterator.__size_of__G") ||
                    Name.starts_with("std.util.option.__size_of__G") ||
                    Name.starts_with("std.util.result.__size_of__G") ||
                    Name.starts_with("std.memory.size_of__G");
  const bool Copy = Name.starts_with("std.util.iterator.__init_copy__G") ||
                    Name.starts_with("std.util.option.__init_copy__G") ||
                    Name.starts_with("std.util.result.__init_copy__G");
  const bool InitDefault = Name.starts_with("std.memory.init_default__G");
  const bool Drop = Name.starts_with("std.util.iterator.__drop_at__G") ||
                    Name.starts_with("std.util.option.__drop_at__G") ||
                    Name.starts_with("std.util.result.__drop_at__G");
  const bool RawCopy = Name.starts_with("std.memory.init_copy__G");
  const bool RawMove = Name.starts_with("std.memory.init_move__G");
  const bool RawAccess = Name.starts_with("std.memory.assume_init__G");
  const bool RawDrop = Name.starts_with("std.memory.drop_init__G");
  if (!Size && !Copy && !InitDefault && !Drop && !RawCopy && !RawMove && !RawAccess && !RawDrop)
    return false;
  if (DeclarationOnly) {
    Func.setPrivate();
    return true;
  }
  Func.setPrivate();
  const auto Loc = GetLocation(Function.Loc);
  auto *Entry = Func.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  const lex::Node *Parameter = nullptr;
  for (const auto &Child : Function.children)
    if (Child->kind == lex::NodeKind::ast_parameter) {
      Parameter = Child.get();
      break;
    }
  assert(Parameter && "memory intrinsic needs a typed pointer parameter");
  auto Type = Analysis.GetType(*Parameter).Pointee();
  if (RawCopy || RawMove || RawAccess || RawDrop) {
    const auto *Raw = Analysis.GetClass(Type);
    assert(Raw && Raw->RawStorage && Raw->Fields.size() == 1 && "raw intrinsic needs Raw<T>");
    Type = Raw->Fields.front().Value;
    if (RawAccess) {
      mlir::func::ReturnOp::create(Builder, Loc, Entry->getArgument(0));
    } else if (RawDrop) {
      if (Type.IsClass())
        mlir::func::CallOp::create(Builder,
                                   Loc,
                                   Analysis.GetClass(Type)->DestructorSymbol,
                                   mlir::TypeRange{},
                                   mlir::ValueRange{Entry->getArgument(0)});
      mlir::func::ReturnOp::create(Builder, Loc);
    } else {
      if (Type.IsClass())
        EmitTransfer(
            *Analysis.GetClass(Type), Entry->getArgument(0), Entry->getArgument(1), RawMove, Loc);
      else {
        auto Value = mlir::LLVM::LoadOp::create(Builder, Loc, GetType(Type), Entry->getArgument(1));
        mlir::LLVM::StoreOp::create(Builder, Loc, Value, Entry->getArgument(0));
      }
      mlir::func::ReturnOp::create(Builder, Loc);
    }
    return true;
  }
  if (Size) {
    std::uint64_t Bytes = 0;
    if (Type.IsClass())
      Bytes = Analysis.GetClass(Type)->Size;
    else if (Type.IsArray()) {
      auto Element = Type;
      std::uint64_t Count = 1;
      while (Element.IsArray()) {
        Count *= Element.ArrayLength();
        Element = Element.Indexed();
      }
      Bytes = Count * (Analysis.GetBitWidth(Element) + 7) / 8;
    } else
      Bytes = (Analysis.GetBitWidth(Type) + 7) / 8;
    auto Value = mlir::arith::ConstantIntOp::create(
        Builder, Loc, Bytes, Analysis.GetTargetLayout().GetPointerBitWidth());
    mlir::func::ReturnOp::create(Builder, Loc, Value.getResult());
  } else if (InitDefault) {
    mlir::func::CallOp::create(Builder,
                               Loc,
                               Analysis.GetClass(Type)->ConstructorSymbol,
                               mlir::TypeRange{},
                               mlir::ValueRange{Entry->getArgument(0)});
    mlir::func::ReturnOp::create(Builder, Loc);
  } else if (Copy) {
    if (Type.IsClass())
      EmitTransfer(
          *Analysis.GetClass(Type), Entry->getArgument(0), Entry->getArgument(1), false, Loc);
    else {
      auto Value = mlir::LLVM::LoadOp::create(Builder, Loc, GetType(Type), Entry->getArgument(1));
      mlir::LLVM::StoreOp::create(Builder, Loc, Value, Entry->getArgument(0));
    }
    mlir::func::ReturnOp::create(Builder, Loc);
  } else {
    if (Type.IsClass())
      mlir::func::CallOp::create(Builder,
                                 Loc,
                                 Analysis.GetClass(Type)->DestructorSymbol,
                                 mlir::TypeRange{},
                                 mlir::ValueRange{Entry->getArgument(0)});
    mlir::func::ReturnOp::create(Builder, Loc);
  }
  return true;
}

bool codegen::IRGen::EmitSliceIntrinsic(const lex::Node &Function, mlir::func::FuncOp Func,
                                        bool DeclarationOnly) {
  const auto Id = Analysis.GetReflection().GetId(Function);
  if (!Id)
    return false;
  const auto &Name = Analysis.GetReflection().Get(*Id).QualifiedName;
  if (!Name.starts_with("std.util.slice.from_raw_parts__G") &&
      !Name.starts_with("std.util.slice.from_raw_parts_const__G"))
    return false;
  Func.setPrivate();
  if (DeclarationOnly)
    return true;
  const auto Loc = GetLocation(Function.Loc);
  auto *Entry = Func.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  auto Result = mlir::LLVM::ZeroOp::create(Builder, Loc, Func.getFunctionType().getResult(0));
  auto WithPointer = mlir::LLVM::InsertValueOp::create(
      Builder, Loc, Result, Entry->getArgument(0), Builder.getDenseI64ArrayAttr({0}));
  auto WithLength = mlir::LLVM::InsertValueOp::create(
      Builder, Loc, WithPointer, Entry->getArgument(1), Builder.getDenseI64ArrayAttr({1}));
  mlir::func::ReturnOp::create(Builder, Loc, WithLength.getResult());
  return true;
}
