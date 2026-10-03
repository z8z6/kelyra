#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>

using namespace kelyra;

codegen::IRGen::Variable *codegen::IRGen::FindVariable(std::string_view Name) {
  for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope) {
    const auto It = Scope->find(std::string(Name));
    if (It != Scope->end())
      return &It->second;
  }
  llvm_unreachable("semantic analysis accepted an unknown variable");
}

mlir::Value codegen::IRGen::CreateAlloca(const sema::Type &Type, mlir::Location Loc) {
  mlir::OpBuilder::InsertionGuard Guard(Builder);
  Builder.setInsertionPointToStart(FunctionEntry);
  auto One = mlir::arith::ConstantIntOp::create(Builder, Loc, 1, 64);
  return mlir::LLVM::AllocaOp::create(Builder,
                                      Loc,
                                      mlir::LLVM::LLVMPointerType::get(&Context),
                                      GetType(Type),
                                      One,
                                      Type.IsClass() ? Analysis.GetClass(Type)->Alignment
                                                     : Analysis.GetAlignment(Type));
}

mlir::Value codegen::IRGen::EmitSliceIndex(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  auto Value = EmitExpression(Expression);
  const auto &Type = Analysis.GetType(Expression);
  if (Analysis.GetBitWidth(Type) == 64)
    return Value;
  if (sema::IsSignedInteger(Type.Element))
    return mlir::arith::ExtSIOp::create(Builder, Loc, Builder.getI64Type(), Value);
  return mlir::arith::ExtUIOp::create(Builder, Loc, Builder.getI64Type(), Value);
}

std::pair<mlir::Value, mlir::Value> codegen::IRGen::EmitSliceParts(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &Type = Analysis.GetType(Expression);
  if (Type.IsSlice()) {
    auto Value = EmitExpression(Expression);
    mlir::Value Length = mlir::LLVM::ExtractValueOp::create(Builder, Loc, Value, 1);
    if (Analysis.GetTargetLayout().GetPointerBitWidth() < 64)
      Length = mlir::arith::ExtUIOp::create(Builder, Loc, Builder.getI64Type(), Length);
    return {mlir::LLVM::ExtractValueOp::create(Builder, Loc, Value, 0), Length};
  }
  auto Address = EmitAddress(Expression);
  auto Zero = mlir::arith::ConstantIntOp::create(Builder, Loc, 0, 32);
  auto Data = mlir::LLVM::GEPOp::create(Builder,
                                        Loc,
                                        mlir::LLVM::LLVMPointerType::get(&Context),
                                        GetType(Type),
                                        Address,
                                        mlir::ValueRange{Zero, Zero});
  auto Length = mlir::arith::ConstantIntOp::create(Builder, Loc, Type.ArrayLength(), 64);
  return {Data, Length};
}

void codegen::IRGen::EmitSliceBoundsCheck(mlir::Value Valid, mlir::Location Loc) {
  auto *Region = Builder.getInsertionBlock()->getParent();
  auto *Failure = new mlir::Block();
  auto *Continue = new mlir::Block();
  Region->push_back(Failure);
  Region->push_back(Continue);
  mlir::cf::CondBranchOp::create(Builder, Loc, Valid, Continue, Failure);
  Builder.setInsertionPointToStart(Failure);
  mlir::LLVM::Trap::create(Builder, Loc);
  mlir::LLVM::UnreachableOp::create(Builder, Loc);
  Builder.setInsertionPointToStart(Continue);
}

mlir::Value codegen::IRGen::EmitAddress(const lex::Node &Expression) {
  using K = lex::NodeKind;
  if (const auto *Field = Analysis.GetExternalField(Expression)) {
    const auto &Base = *Expression.children.front();
    const auto &BaseType = Analysis.GetType(Base);
    const auto Loc = GetLocation(Expression.Loc);
    mlir::Value Address;
    if (BaseType.IsPointer()) {
      Address = EmitExpression(Base);
    } else if (Base.kind == K::ast_name || Base.kind == K::ast_member ||
               Base.kind == K::ast_index || (Base.kind == K::ast_unary && Base.text == "*")) {
      Address = EmitAddress(Base);
    } else {
      Address = CreateAlloca(BaseType, Loc);
      mlir::LLVM::StoreOp::create(Builder, Loc, EmitExpression(Base), Address);
    }
    auto Offset = mlir::arith::ConstantIntOp::create(Builder, Loc, Field->OffsetBits / 8, 64);
    return mlir::LLVM::GEPOp::create(Builder,
                                     Loc,
                                     mlir::LLVM::LLVMPointerType::get(&Context),
                                     Builder.getI8Type(),
                                     Address,
                                     mlir::ValueRange{Offset});
  }
  if (const auto *Field = Analysis.GetStaticField(Expression))
    return mlir::LLVM::AddressOfOp::create(Builder,
                                           GetLocation(Expression.Loc),
                                           mlir::LLVM::LLVMPointerType::get(&Context),
                                           Field->Symbol);
  if (Analysis.GetField(Expression)) {
    mlir::Value Address;
    const sema::ClassInfo *Owner = CurrentClass;
    if (Expression.kind == K::ast_member) {
      const auto &Base = *Expression.children.front();
      const auto &BaseType = Analysis.GetType(Base);
      Owner = Analysis.GetFieldOwner(Expression);
      Address = BaseType.IsPointer() ? EmitExpression(Base) : EmitAddress(Base);
    } else {
      Address = FindVariable("this")->DirectValue;
    }
    return FieldAddress(
        *Owner, Address, Analysis.GetFieldIndex(Expression), GetLocation(Expression.Loc));
  }
  if (Expression.kind == K::ast_name)
    return FindVariable(Expression.text)->Address;
  if (Expression.kind == K::ast_group)
    return EmitAddress(*Expression.children.front());
  if (Expression.kind == K::ast_unary && Expression.text == "*")
    return EmitExpression(*Expression.children.front());

  const auto &Base = *Expression.children[0];
  const auto BaseType = Analysis.GetType(Base);
  if (BaseType.IsSlice()) {
    auto [Data, Length] = EmitSliceParts(Base);
    auto Index = EmitSliceIndex(*Expression.children[1]);
    auto Valid = mlir::arith::CmpIOp::create(
        Builder, GetLocation(Expression.Loc), mlir::arith::CmpIPredicate::ult, Index, Length);
    EmitSliceBoundsCheck(Valid, GetLocation(Expression.Loc));
    return mlir::LLVM::GEPOp::create(Builder,
                                     GetLocation(Expression.Loc),
                                     mlir::LLVM::LLVMPointerType::get(&Context),
                                     GetType(BaseType.Indexed()),
                                     Data,
                                     mlir::ValueRange{Index});
  }
  auto BaseAddress = EmitAddress(Base);
  const auto &IndexExpression = *Expression.children[1];
  const auto &IndexType = Analysis.GetType(IndexExpression);
  auto Index = EmitExpression(IndexExpression);
  if (SafeLevel > 0) {
    const auto BitWidth = Analysis.GetBitWidth(IndexType);
    mlir::Value Valid;
    if (sema::IsSignedInteger(IndexType.Element)) {
      auto Zero = mlir::arith::ConstantIntOp::create(
          Builder, GetLocation(IndexExpression.Loc), GetType(IndexType), llvm::APInt(BitWidth, 0));
      Valid = mlir::arith::CmpIOp::create(
          Builder, GetLocation(IndexExpression.Loc), mlir::arith::CmpIPredicate::sge, Index, Zero);
    }
    const auto Length = BaseType.Dimensions.front();
    const bool LengthFits = BitWidth >= 64 || Length < (std::uint64_t{1} << BitWidth);
    if (LengthFits) {
      auto Limit = mlir::arith::ConstantIntOp::create(Builder,
                                                      GetLocation(IndexExpression.Loc),
                                                      GetType(IndexType),
                                                      llvm::APInt(BitWidth, Length));
      auto BelowLimit = mlir::arith::CmpIOp::create(
          Builder, GetLocation(IndexExpression.Loc), mlir::arith::CmpIPredicate::ult, Index, Limit);
      if (Valid)
        Valid = mlir::arith::AndIOp::create(
            Builder, GetLocation(IndexExpression.Loc), Valid, BelowLimit);
      else
        Valid = BelowLimit;
    }
    if (Valid)
      mlir::cf::AssertOp::create(
          Builder, GetLocation(IndexExpression.Loc), Valid, "array index out of bounds");
  }
  auto Zero = mlir::arith::ConstantIntOp::create(Builder, GetLocation(Expression.Loc), 0, 32);
  llvm::SmallVector<mlir::Value> Indices{Zero, Index};
  return mlir::LLVM::GEPOp::create(Builder,
                                   GetLocation(Expression.Loc),
                                   mlir::LLVM::LLVMPointerType::get(&Context),
                                   GetType(BaseType),
                                   BaseAddress,
                                   Indices);
}
