#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

using namespace kelyra;

void codegen::IRGen::EmitSingletonAccessor(const sema::ClassInfo &Class,
                                           bool DeclarationOnly) {
  const auto Loc = GetLocation(Class.Node->Loc);
  const auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  const auto I32 = Builder.getI32Type();
  mlir::LLVM::GlobalOp Storage;
  mlir::LLVM::GlobalOp State;
  if (!DeclarationOnly) {
    const auto Linkage = Class.Node->GenericInstance
                             ? mlir::LLVM::Linkage::LinkonceODR
                             : mlir::LLVM::Linkage::Private;
    const auto Bytes =
        mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(), Class.Size);
    Storage = mlir::LLVM::GlobalOp::create(
        Builder, Loc, Bytes, false, Linkage, Class.InstanceSymbol + ".storage",
        Builder.getStringAttr(std::string(Class.Size, '\0')), Class.Alignment);
    State = mlir::LLVM::GlobalOp::create(
        Builder, Loc, I32, false, Linkage, Class.InstanceSymbol + ".state",
        Builder.getI32IntegerAttr(0), alignof(std::uint32_t));
  }
  auto Function =
      mlir::func::FuncOp::create(Builder, Loc, Class.InstanceSymbol,
                                 Builder.getFunctionType({}, {Pointer}));
  if (Class.Node->GenericInstance)
    Function->setAttr("kelyra.generic", Builder.getUnitAttr());
  if (DeclarationOnly || !Class.Public)
    Function.setPrivate();
  if (DeclarationOnly)
    return;

  auto *Entry = Function.addEntryBlock();
  auto *Loop = new mlir::Block();
  auto *Claim = new mlir::Block();
  auto *Initialize = new mlir::Block();
  auto *Ready = new mlir::Block();
  Function.getBody().push_back(Loop);
  Function.getBody().push_back(Claim);
  Function.getBody().push_back(Initialize);
  Function.getBody().push_back(Ready);
  Builder.setInsertionPointToStart(Entry);
  auto StorageAddress = mlir::LLVM::AddressOfOp::create(
      Builder, Loc, Pointer, Storage.getSymNameAttr());
  auto StateAddress = mlir::LLVM::AddressOfOp::create(Builder, Loc, Pointer,
                                                      State.getSymNameAttr());
  auto Zero = mlir::arith::ConstantIntOp::create(Builder, Loc, 0, 32);
  auto One = mlir::arith::ConstantIntOp::create(Builder, Loc, 1, 32);
  auto Two = mlir::arith::ConstantIntOp::create(Builder, Loc, 2, 32);
  mlir::cf::BranchOp::create(Builder, Loc, Loop);

  Builder.setInsertionPointToStart(Loop);
  auto Loaded = mlir::LLVM::LoadOp::create(
      Builder, Loc, I32, StateAddress, alignof(std::uint32_t), false, false,
      false, false, mlir::LLVM::AtomicOrdering::acquire);
  auto IsReady = mlir::arith::CmpIOp::create(
      Builder, Loc, mlir::arith::CmpIPredicate::eq, Loaded, Two);
  mlir::cf::CondBranchOp::create(Builder, Loc, IsReady, Ready, Claim);

  Builder.setInsertionPointToStart(Claim);
  auto Claimed = mlir::LLVM::AtomicCmpXchgOp::create(
      Builder, Loc, StateAddress, Zero, One,
      mlir::LLVM::AtomicOrdering::acq_rel, mlir::LLVM::AtomicOrdering::acquire,
      llvm::StringRef(), alignof(std::uint32_t));
  auto Won =
      mlir::LLVM::ExtractValueOp::create(Builder, Loc, Claimed, std::size_t{1});
  mlir::cf::CondBranchOp::create(Builder, Loc, Won, Initialize, Loop);

  Builder.setInsertionPointToStart(Initialize);
  mlir::func::CallOp::create(Builder, Loc, Class.ConstructorSymbol,
                             mlir::TypeRange{},
                             mlir::ValueRange{StorageAddress});
  mlir::LLVM::StoreOp::create(Builder, Loc, Two, StateAddress,
                              alignof(std::uint32_t), false, false, false,
                              mlir::LLVM::AtomicOrdering::release);
  mlir::cf::BranchOp::create(Builder, Loc, Ready);

  Builder.setInsertionPointToStart(Ready);
  mlir::func::ReturnOp::create(Builder, Loc, StorageAddress.getResult());
}

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
  if (const auto *Constructor = Analysis.GetInlineConstructor(Expression)) {
    const auto *Class = Analysis.GetConstructorCall(Expression);
    if (!Class)
      Class = Analysis.GetBaseConstructorCall(Expression);
    Scopes.emplace_back();
    Cleanups.emplace_back();
    for (std::size_t I = 1; I < Expression.children.size(); ++I) {
      const auto &Argument = *Expression.children[I];
      const auto &Type = Analysis.GetType(Argument);
      mlir::Value Storage;
      if (Type.IsClass()) {
        Storage = EmitClassSourceAddress(Argument).first;
      } else {
        Storage = CreateAlloca(Type, GetLocation(Argument.Loc));
        mlir::LLVM::StoreOp::create(Builder, GetLocation(Argument.Loc),
                                    EmitExpression(Argument), Storage);
      }
      Scopes.back().emplace("$forward" + std::to_string(I - 1),
                            Variable{Type, Storage, {}});
    }
    sema::Type Receiver{sema::BuiltinType::Class, {}};
    Receiver.ClassName = Class->QualifiedName;
    Receiver.AddPointer();
    Scopes.back().emplace("this", Variable{Receiver, {}, Address});
    const auto *PreviousClass = CurrentClass;
    const auto PreviousTransfer = InTransferConstructor;
    const auto *PreviousBlock = InlineConstructorBlock;
    CurrentClass = Class;
    InTransferConstructor = true;
    for (const auto &Child : Constructor->children)
      if (Child->kind == lex::TokenKind::ast_block) {
        InlineConstructorBlock = Child.get();
        if (Class->UserFieldCount == 0)
          EmitVirtualSlots(*Class, Address, GetLocation(Expression.Loc));
        EmitBlock(*Child);
        break;
      }
    InlineConstructorBlock = PreviousBlock;
    InTransferConstructor = PreviousTransfer;
    CurrentClass = PreviousClass;
    Cleanups.pop_back();
    Scopes.pop_back();
    return;
  }
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
  if (Analysis.IsForwardTemporary(Expression))
    return {EmitAddress(Expression), true};
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
