#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

using namespace kelyra;

bool codegen::IRGen::EmitOptionIntrinsic(const lex::Node &Function,
                                         mlir::func::FuncOp Func,
                                         bool DeclarationOnly) {
  const auto Id = Analysis.GetReflection().GetId(Function);
  if (!Id)
    return false;
  const auto &Name = Analysis.GetReflection().Get(*Id).QualifiedName;
  const bool Size = Name.starts_with("std.option.__size_of__G") ||
                    Name.starts_with("std.result.__size_of__G");
  const bool Copy = Name.starts_with("std.option.__init_copy__G") ||
                    Name.starts_with("std.result.__init_copy__G");
  const bool Drop = Name.starts_with("std.option.__drop_at__G") ||
                    Name.starts_with("std.result.__drop_at__G");
  if (!Size && !Copy && !Drop)
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
    if (Child->kind == lex::TokenKind::ast_parameter) {
      Parameter = Child.get();
      break;
    }
  assert(Parameter && "Option intrinsic needs a typed pointer parameter");
  const auto Type = Analysis.GetType(*Parameter).Pointee();
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
      Bytes = Count * (sema::GetBitWidth(Element) + 7) / 8;
    } else
      Bytes = (sema::GetBitWidth(Type) + 7) / 8;
    auto Value = mlir::arith::ConstantIntOp::create(Builder, Loc, Bytes, 64);
    mlir::func::ReturnOp::create(Builder, Loc, Value.getResult());
  } else if (Copy) {
    if (Type.IsClass())
      EmitTransfer(*Analysis.GetClass(Type), Entry->getArgument(0),
                   Entry->getArgument(1), false, Loc);
    else {
      auto Value = mlir::LLVM::LoadOp::create(Builder, Loc, GetType(Type),
                                              Entry->getArgument(1));
      mlir::LLVM::StoreOp::create(Builder, Loc, Value, Entry->getArgument(0));
    }
    mlir::func::ReturnOp::create(Builder, Loc);
  } else {
    if (Type.IsClass())
      mlir::func::CallOp::create(
          Builder, Loc, Analysis.GetClass(Type)->DestructorSymbol,
          mlir::TypeRange{}, mlir::ValueRange{Entry->getArgument(0)});
    mlir::func::ReturnOp::create(Builder, Loc);
  }
  return true;
}
