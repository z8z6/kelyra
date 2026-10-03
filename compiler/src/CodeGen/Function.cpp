#include "CodeGen/IRGen.h"
#include "Support/BuiltinAnnotation.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>

using namespace kelyra;

namespace {
bool IsTypeNode(lex::NodeKind Kind) {
  using K = lex::NodeKind;
  return Kind == K::ast_type || Kind == K::ast_pointer_type || Kind == K::ast_array_type ||
         Kind == K::ast_slice_type || Kind == K::ast_result_types || Kind == K::ast_function_type;
}

bool HasTerminator(mlir::Block *Block) {
  return !Block->empty() && Block->back().hasTrait<mlir::OpTrait::IsTerminator>();
}
} // namespace

void codegen::IRGen::EmitFunction(const lex::Node &Function, const sema::ClassInfo *Owner,
                                  bool DeclarationOnly) {
  using K = lex::NodeKind;
  llvm::SmallVector<const lex::Node *> Parameters;
  const lex::Node *ReturnType = nullptr;
  const lex::Node *Body = nullptr;
  for (const auto &Child : Function.children) {
    if (Child->kind == K::ast_parameter)
      Parameters.push_back(Child.get());
    else if (IsTypeNode(Child->kind) && !Analysis.GetType(*Child).IsVoid())
      ReturnType = Child.get();
    else if (Child->kind == K::ast_block)
      Body = Child.get();
  }

  llvm::SmallVector<mlir::Type> ParameterTypes;
  const bool Static =
      Owner &&
      std::any_of(Function.children.begin(), Function.children.end(), [](const auto &Part) {
        return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "static");
      });
  if (Owner && !Static)
    ParameterTypes.push_back(mlir::LLVM::LLVMPointerType::get(&Context));
  for (const auto *Parameter : Parameters)
    ParameterTypes.push_back(GetType(Analysis.GetType(*Parameter)));
  llvm::SmallVector<mlir::Type> Results;
  if (ReturnType)
    Results.push_back(GetType(Analysis.GetType(*ReturnType)));
  auto FunctionType = Builder.getFunctionType(ParameterTypes, Results);
  auto Func = mlir::func::FuncOp::create(
      Builder, GetLocation(Function.Loc), Analysis.GetSymbol(Function), FunctionType);
  if (Function.GenericInstance || (Owner && Owner->Node->GenericInstance))
    Func->setAttr("kelyra.generic", Builder.getUnitAttr());
  if (EmitOptionIntrinsic(Function, Func, DeclarationOnly))
    return;
  if (EmitReflectIntrinsic(Function, Func, DeclarationOnly))
    return;
  if (EmitSliceIntrinsic(Function, Func, DeclarationOnly))
    return;
  for (const auto &Annotation : Analysis.GetAnnotations(Function))
    if (Annotation.Name == "std.annotation.inline" && !Annotation.Arguments.empty() &&
        Annotation.Arguments.front().Value.Text == "std.annotation.InlineMode.Always")
      Func->setAttr("kelyra.always_inline", Builder.getUnitAttr());
  if (std::any_of(
          Analysis.GetAnnotations(Function).begin(),
          Analysis.GetAnnotations(Function).end(),
          [](const auto &Annotation) { return Annotation.Name == "std.annotation.intrinsic"; })) {
    Func.setPrivate();
    if (!DeclarationOnly) {
      auto *Entry = Func.addEntryBlock();
      Builder.setInsertionPointToStart(Entry);
      const auto Loc = GetLocation(Function.Loc);
      mlir::LLVM::Trap::create(Builder, Loc);
      mlir::LLVM::UnreachableOp::create(Builder, Loc);
    }
    return;
  }
  if (DeclarationOnly || !Body) {
    // The definition lives in a linked library; emit only the declaration.
    Func.setPrivate();
    return;
  }
  CurrentClass = Owner;
  BeginDebugFunction(Func, Function);
  const bool IsMain =
      std::any_of(Function.children.begin(), Function.children.end(), [](const auto &Part) {
        return Part->kind == lex::NodeKind::ast_annotation &&
               IsBuiltinAnnotation(Part->text, "main");
      });
  if (!Analysis.IsPublic(Function) && !IsMain)
    Func.setPrivate();
  auto *Entry = Func.addEntryBlock();
  FunctionEntry = Entry;
  Builder.setInsertionPointToStart(Entry);
  Scopes.clear();
  Scopes.emplace_back();
  Cleanups.clear();
  Cleanups.emplace_back();
  Loops.clear();
  CurrentClass = Owner;
  ActiveDestructor = Function.kind == K::ast_destructor ? Owner : nullptr;
  InTransferConstructor = Owner && (Function.kind == K::ast_constructor ||
                                    Function.text == "copy" || Function.text == "move");
  const unsigned Offset = Owner && !Static ? 1 : 0;
  if (Owner && !Static) {
    sema::Type Receiver{sema::BuiltinType::Class, {}};
    Receiver.ClassName = Owner->QualifiedName;
    Receiver.AddPointer();
    Scopes.back().emplace("this", Variable{Receiver, {}, Entry->getArgument(0)});
    if (DebugInfo) {
      auto Address = CreateAlloca(Receiver, GetLocation(Function.Loc));
      mlir::LLVM::StoreOp::create(
          Builder, GetLocation(Function.Loc), Entry->getArgument(0), Address);
      EmitDebugVariable("this", Function.Loc, Receiver, Address, 1);
    }
  }
  if (ActiveDestructor)
    EmitVirtualSlots(*Owner, Entry->getArgument(0), GetLocation(Function.Loc));
  if (InTransferConstructor && Owner->UserFieldCount == 0)
    EmitVirtualSlots(*Owner, Entry->getArgument(0), GetLocation(Function.Loc));
  for (std::size_t I = 0; I < Parameters.size(); ++I) {
    const auto Type = Analysis.GetType(*Parameters[I]);
    if (!Type.IsRecord() && Analysis.GetBitWidth(Type) > 128) {
      Scopes.back().emplace(Parameters[I]->text,
                            Variable{Type, {}, Entry->getArgument(I + Offset)});
      EmitDebugVariable(Parameters[I]->text,
                        Parameters[I]->Loc,
                        Type,
                        Entry->getArgument(I + Offset),
                        I + Offset + 1,
                        true);
      continue;
    }
    auto Address = CreateAlloca(Type, GetLocation(Parameters[I]->Loc));
    mlir::LLVM::StoreOp::create(
        Builder, GetLocation(Parameters[I]->Loc), Entry->getArgument(I + Offset), Address);
    Scopes.back().emplace(Parameters[I]->text, Variable{Type, Address, {}});
    if (Type.IsClass())
      Cleanups.front().push_back({Analysis.GetClass(Type), Address});
    EmitDebugVariable(Parameters[I]->text, Parameters[I]->Loc, Type, Address, I + Offset + 1);
  }
  EmitBlock(*Body);
  if (!HasTerminator(Builder.getInsertionBlock())) {
    if (ReturnType)
      mlir::LLVM::UnreachableOp::create(Builder, GetLocation(Body->Loc));
    else {
      EmitCleanups(0, GetLocation(Body->Loc));
      if (ActiveDestructor)
        EmitFieldDestructors(*ActiveDestructor, Entry->getArgument(0), GetLocation(Body->Loc));
      mlir::func::ReturnOp::create(Builder, GetLocation(Body->Loc));
    }
  }
  CurrentClass = nullptr;
  ActiveDestructor = nullptr;
  InTransferConstructor = false;
  DebugScope = {};
}

void codegen::IRGen::EmitExternalDeclarations() {
  for (const auto &External : Analysis.GetExternalFunctions()) {
    if (External.Variadic || External.Return.IsRecord() ||
        std::any_of(External.Parameters.begin(),
                    External.Parameters.end(),
                    [](const sema::Type &Type) { return Type.IsRecord(); }))
      continue;
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : External.Parameters)
      Parameters.push_back(GetType(Parameter));
    llvm::SmallVector<mlir::Type> Results;
    if (!External.Return.IsVoid())
      Results.push_back(GetType(External.Return));
    auto Function = mlir::func::FuncOp::create(Builder,
                                               Builder.getUnknownLoc(),
                                               External.Name,
                                               Builder.getFunctionType(Parameters, Results));
    Function.setPrivate();
  }
  for (const auto &Wrapper : Analysis.GetCWrappers()) {
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : Wrapper.Parameters)
      Parameters.push_back(GetType(Parameter));
    llvm::SmallVector<mlir::Type> Results;
    if (!Wrapper.ReturnByAddress && !Wrapper.Return.IsVoid())
      Results.push_back(GetType(Wrapper.Return));
    auto Function = mlir::func::FuncOp::create(Builder,
                                               Builder.getUnknownLoc(),
                                               Wrapper.Name,
                                               Builder.getFunctionType(Parameters, Results));
    Function.setPrivate();
  }
}
