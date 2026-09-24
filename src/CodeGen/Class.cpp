#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

using namespace kelyra;

mlir::Value codegen::IRGen::FieldAddress(const sema::ClassInfo &Class,
                                         mlir::Value Address, std::size_t Index,
                                         mlir::Location Loc) {
  sema::Type Type{sema::BuiltinType::Class, {}};
  Type.ClassName = Class.QualifiedName;
  llvm::SmallVector<mlir::LLVM::GEPArg> Indices{
      0, static_cast<int32_t>(Class.Fields[Index].LayoutIndex)};
  return mlir::LLVM::GEPOp::create(Builder, Loc,
                                   mlir::LLVM::LLVMPointerType::get(&Context),
                                   GetType(Type), Address, Indices);
}

void codegen::IRGen::EmitConstruction(const lex::Node &Expression,
                                      mlir::Value Address) {
  llvm::SmallVector<mlir::Value> Arguments{Address};
  for (std::size_t I = 1; I < Expression.children.size(); ++I)
    Arguments.push_back(Analysis.GetType(*Expression.children[I]).IsClass()
                            ? EmitClassArgument(*Expression.children[I])
                            : EmitExpression(*Expression.children[I]));
  mlir::func::CallOp::create(Builder, GetLocation(Expression.Loc),
                             Analysis.GetCallee(Expression), mlir::TypeRange{},
                             Arguments);
}

std::pair<mlir::Value, bool>
codegen::IRGen::EmitClassSourceAddress(const lex::Node &Expression) {
  if (Expression.kind == lex::TokenKind::ast_group &&
      Expression.children.size() == 1)
    return EmitClassSourceAddress(*Expression.children.front());
  if (!Analysis.IsClassTemporary(Expression))
    return {EmitAddress(Expression), false};
  auto Address =
      CreateAlloca(Analysis.GetType(Expression), GetLocation(Expression.Loc));
  if (Analysis.GetConstructorCall(Expression))
    EmitConstruction(Expression, Address);
  else
    mlir::LLVM::StoreOp::create(Builder, GetLocation(Expression.Loc),
                                EmitExpression(Expression), Address);
  return {Address, true};
}

mlir::Value codegen::IRGen::EmitClassArgument(const lex::Node &Expression) {
  const auto &Class = *Analysis.GetClass(Analysis.GetType(Expression));
  const auto Loc = GetLocation(Expression.Loc);
  auto [Source, Temporary] = EmitClassSourceAddress(Expression);
  auto Destination = CreateAlloca(Analysis.GetType(Expression), Loc);
  EmitTransfer(Class, Destination, Source, Temporary, Loc);
  if (Temporary)
    mlir::func::CallOp::create(Builder, Loc, Class.DestructorSymbol,
                               mlir::TypeRange{}, mlir::ValueRange{Source});
  return mlir::LLVM::LoadOp::create(
      Builder, Loc, GetType(Analysis.GetType(Expression)), Destination);
}

void codegen::IRGen::EmitCleanups(std::size_t KeepDepth, mlir::Location Loc) {
  for (auto I = Cleanups.size(); I > KeepDepth; --I)
    for (auto It = Cleanups[I - 1].rbegin(); It != Cleanups[I - 1].rend(); ++It)
      mlir::func::CallOp::create(Builder, Loc, It->Class->DestructorSymbol,
                                 mlir::TypeRange{},
                                 mlir::ValueRange{It->Address});
}

void codegen::IRGen::EmitFieldDestructors(const sema::ClassInfo &Class,
                                          mlir::Value Address,
                                          mlir::Location Loc) {
  for (std::size_t I = Class.Fields.size(); I > 0; --I) {
    const auto &Field = Class.Fields[I - 1];
    if (!Field.Value.IsClass())
      continue;
    const auto *Child = Analysis.GetClass(Field.Value);
    auto ChildAddress = FieldAddress(Class, Address, I - 1, Loc);
    mlir::func::CallOp::create(Builder, Loc, Child->DestructorSymbol,
                               mlir::TypeRange{},
                               mlir::ValueRange{ChildAddress});
  }
}

void codegen::IRGen::EmitVirtualSlots(const sema::ClassInfo &Class,
                                      mlir::Value Address, mlir::Location Loc) {
  if (!Class.BaseName.empty())
    EmitVirtualSlots(*Analysis.GetClass(Class.BaseName), Address, Loc);
  const auto SetSlot = [&](const sema::ClassInfo &Owner, std::size_t Index,
                           const lex::Node &Method) {
    const auto &Signature = Owner.Fields[Index].Value;
    llvm::SmallVector<mlir::Type> Parameters;
    llvm::SmallVector<mlir::Type> Results;
    for (const auto &Parameter : Signature.Parameters)
      Parameters.push_back(GetType(Parameter));
    if (!Signature.Results.front().IsVoid())
      Results.push_back(GetType(Signature.Results.front()));
    auto Value = mlir::func::ConstantOp::create(
        Builder, Loc, Builder.getFunctionType(Parameters, Results),
        mlir::FlatSymbolRefAttr::get(&Context, Analysis.GetSymbol(Method)));
    auto Pointer = mlir::UnrealizedConversionCastOp::create(
        Builder, Loc, GetType(Signature), Value.getResult());
    mlir::LLVM::StoreOp::create(Builder, Loc, Pointer.getResult(0),
                                FieldAddress(Owner, Address, Index, Loc));
  };
  for (const auto &[Name, Index] : Class.VirtualSlots)
    for (const auto &Member : Class.Node->children)
      if (Member->kind == lex::TokenKind::ast_function && Member->text == Name)
        SetSlot(Class, Index, *Member);
  for (const auto &[Name, Slot] : Class.OverrideSlots)
    for (const auto &Member : Class.Node->children)
      if (Member->kind == lex::TokenKind::ast_function && Member->text == Name)
        SetSlot(*Analysis.GetClass(Slot.first), Slot.second, *Member);
}

void codegen::IRGen::EmitDefaultConstructor(const sema::ClassInfo &Class) {
  const auto Loc = GetLocation(Class.Node->Loc);
  auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  auto Function =
      mlir::func::FuncOp::create(Builder, Loc, Class.ConstructorSymbol,
                                 Builder.getFunctionType({Pointer}, {}));
  if (Class.Node->GenericInstance)
    Function->setAttr("kelyra.generic", Builder.getUnitAttr());
  if (!Class.Public)
    Function.setPrivate();
  auto *Entry = Function.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  for (std::size_t I = 0; I < Class.Fields.size(); ++I) {
    const auto &Field = Class.Fields[I];
    const auto Address = FieldAddress(Class, Entry->getArgument(0), I, Loc);
    if (Field.Value.IsClass()) {
      const auto *Child = Analysis.GetClass(Field.Value);
      mlir::func::CallOp::create(Builder, Loc, Child->ConstructorSymbol,
                                 mlir::TypeRange{}, mlir::ValueRange{Address});
    } else {
      auto Zero = mlir::LLVM::ZeroOp::create(Builder, Loc, GetType(Field.Value))
                      .getRes();
      mlir::LLVM::StoreOp::create(Builder, Loc, Zero, Address);
    }
  }
  EmitVirtualSlots(Class, Entry->getArgument(0), Loc);
  mlir::func::ReturnOp::create(Builder, Loc);
}

void codegen::IRGen::EmitDefaultConstructorDeclaration(
    const sema::ClassInfo &Class) {
  auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  auto Function = mlir::func::FuncOp::create(
      Builder, GetLocation(Class.Node->Loc), Class.ConstructorSymbol,
      Builder.getFunctionType({Pointer}, {}));
  Function.setPrivate();
}

void codegen::IRGen::EmitDefaultDestructor(const sema::ClassInfo &Class) {
  const auto Loc = GetLocation(Class.Node->Loc);
  auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  auto Function =
      mlir::func::FuncOp::create(Builder, Loc, Class.DestructorSymbol,
                                 Builder.getFunctionType({Pointer}, {}));
  if (Class.Node->GenericInstance)
    Function->setAttr("kelyra.generic", Builder.getUnitAttr());
  Function.setPrivate();
  auto *Entry = Function.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  EmitVirtualSlots(Class, Entry->getArgument(0), Loc);
  EmitFieldDestructors(Class, Entry->getArgument(0), Loc);
  mlir::func::ReturnOp::create(Builder, Loc);
}

void codegen::IRGen::EmitDefaultDestructorDeclaration(
    const sema::ClassInfo &Class) {
  auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  auto Function = mlir::func::FuncOp::create(
      Builder, GetLocation(Class.Node->Loc), Class.DestructorSymbol,
      Builder.getFunctionType({Pointer}, {}));
  Function.setPrivate();
}

void codegen::IRGen::EmitTransfer(const sema::ClassInfo &Class,
                                  mlir::Value Target, mlir::Value Source,
                                  bool Move, mlir::Location Loc) {
  mlir::func::CallOp::create(
      Builder, Loc, Move ? Class.MoveSymbol : Class.CopySymbol,
      mlir::TypeRange{}, mlir::ValueRange{Target, Source});
}

void codegen::IRGen::EmitDefaultTransfer(const sema::ClassInfo &Class,
                                         bool Move, bool DeclarationOnly) {
  const auto Loc = GetLocation(Class.Node->Loc);
  auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  auto Function = mlir::func::FuncOp::create(
      Builder, Loc, Move ? Class.MoveSymbol : Class.CopySymbol,
      Builder.getFunctionType({Pointer, Pointer}, {}));
  if (Class.Node->GenericInstance)
    Function->setAttr("kelyra.generic", Builder.getUnitAttr());
  if (DeclarationOnly) {
    Function.setPrivate();
    return;
  }
  if (!Class.Public)
    Function.setPrivate();
  auto *Entry = Function.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  for (std::size_t I = 0; I < Class.Fields.size(); ++I) {
    const auto &Field = Class.Fields[I];
    auto Target = FieldAddress(Class, Entry->getArgument(0), I, Loc);
    auto Source = FieldAddress(Class, Entry->getArgument(1), I, Loc);
    if (Field.Value.IsClass()) {
      EmitTransfer(*Analysis.GetClass(Field.Value), Target, Source, Move, Loc);
    } else {
      auto Value = mlir::LLVM::LoadOp::create(Builder, Loc,
                                              GetType(Field.Value), Source);
      mlir::LLVM::StoreOp::create(Builder, Loc, Value, Target);
      if (Move) {
        auto Zero =
            mlir::LLVM::ZeroOp::create(Builder, Loc, GetType(Field.Value));
        mlir::LLVM::StoreOp::create(Builder, Loc, Zero, Source);
      }
    }
  }
  mlir::func::ReturnOp::create(Builder, Loc);
}
