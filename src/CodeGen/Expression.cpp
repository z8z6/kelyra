#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cassert>
#include <unordered_map>

using namespace kelyra;
using K = lex::TokenKind;

mlir::Value codegen::IRGen::EmitExpression(const lex::Node &Expression) {
  if (const auto *Constant = Analysis.GetConstant(Expression))
    return EmitExpression(*Constant);
  if (const auto *Constant = Analysis.GetExternalConstant(Expression)) {
    const auto Width = sema::GetBitWidth(Constant->Value);
    const auto Negative = Constant->Integer.starts_with('-');
    const llvm::StringRef Digits(Constant->Integer.data() + (Negative ? 1 : 0),
                                 Constant->Integer.size() - (Negative ? 1 : 0));
    auto Value = llvm::APInt(Width, Digits, 10);
    if (Negative)
      Value = -Value;
    return mlir::arith::ConstantIntOp::create(
        Builder, GetLocation(Expression.Loc), GetType(Constant->Value), Value);
  }
  if (const auto *Variant = Analysis.GetEnumVariant(Expression)) {
    const auto &Type = Analysis.GetType(Expression);
    const bool Negative = Variant->Value.starts_with('-');
    const llvm::StringRef Digits(Variant->Value.data() + (Negative ? 1 : 0),
                                 Variant->Value.size() - (Negative ? 1 : 0));
    auto Value = llvm::APInt(sema::GetBitWidth(Type), Digits, 10);
    if (Negative)
      Value = -Value;
    return mlir::arith::ConstantIntOp::create(
        Builder, GetLocation(Expression.Loc), GetType(Type), Value);
  }
  if (const auto *Symbol = Analysis.GetFunctionValue(Expression)) {
    const auto &Type = Analysis.GetType(Expression);
    llvm::SmallVector<mlir::Type> Parameters;
    llvm::SmallVector<mlir::Type> Results;
    for (const auto &Parameter : Type.Parameters)
      Parameters.push_back(GetType(Parameter));
    if (!Type.Results.front().IsVoid())
      Results.push_back(GetType(Type.Results.front()));
    const auto Loc = GetLocation(Expression.Loc);
    auto Function = mlir::func::ConstantOp::create(
        Builder, Loc, Builder.getFunctionType(Parameters, Results),
        mlir::FlatSymbolRefAttr::get(&Context, *Symbol));
    return mlir::UnrealizedConversionCastOp::create(Builder, Loc, GetType(Type),
                                                    Function.getResult())
        .getResult(0);
  }
  using K = lex::TokenKind;
  using Handler = mlir::Value (IRGen::*)(const lex::Node &);
  static const std::unordered_map<K, Handler> Handlers = {
      {K::ast_name, &IRGen::EmitNameExpression},
      {K::ast_index, &IRGen::EmitIndexExpression},
      {K::ast_slice, &IRGen::EmitSliceExpression},
      {K::ast_member, &IRGen::EmitIndexExpression},
      {K::ast_call, &IRGen::EmitCallExpression},
      {K::ast_literal, &IRGen::EmitLiteralExpression},
      {K::ast_group, &IRGen::EmitGroupExpression},
      {K::ast_unary, &IRGen::EmitUnaryExpression},
      {K::ast_binary, &IRGen::EmitBinaryExpression},
      {K::ast_cast, &IRGen::EmitCastExpression},
      {K::ast_block_expr, &IRGen::EmitBlockExpression},
      {K::ast_match, &IRGen::EmitMatchExpression},
  };
  const auto It = Handlers.find(Expression.kind);
  assert(It != Handlers.end() &&
         "semantic analysis accepted an expression without an IR handler");
  auto Value = (this->*It->second)(Expression);
  if (const auto *Interface = Analysis.GetInterfaceConversion(Expression))
    return EmitInterfaceConversion(Expression, Value,
                                   *Analysis.GetClass(*Interface));
  return Value;
}

mlir::Value
codegen::IRGen::EmitInterfaceConversion(const lex::Node &Expression,
                                        mlir::Value Object,
                                        const sema::ClassInfo &Interface) {
  const auto Loc = GetLocation(Expression.Loc);
  sema::Type InterfaceType{sema::BuiltinType::Class, {}};
  InterfaceType.ClassName = Interface.QualifiedName;
  InterfaceType.AddPointer();
  const auto *Concrete =
      Analysis.GetClass(Analysis.GetType(Expression).ClassName);
  const bool FromInterface = Concrete->IsInterface;
  auto Source = Object;
  if (FromInterface)
    Object = mlir::LLVM::ExtractValueOp::create(Builder, Loc, Source,
                                                std::size_t{0});
  auto Value = mlir::LLVM::UndefOp::create(Builder, Loc, GetType(InterfaceType))
                   .getResult();
  Value = mlir::LLVM::InsertValueOp::create(Builder, Loc, Value, Object,
                                            Builder.getDenseI64ArrayAttr({0}));
  auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
  for (std::size_t I = 0; I < Interface.InterfaceMethods.size(); ++I) {
    const auto &MethodKey = Interface.InterfaceMethods[I];
    const auto Name = MethodKey.substr(MethodKey.rfind('.') + 1);
    if (FromInterface) {
      const auto Method = std::find_if(
          Concrete->InterfaceMethods.begin(), Concrete->InterfaceMethods.end(),
          [&](const auto &Candidate) {
            return Candidate.substr(Candidate.rfind('.') + 1) == Name;
          });
      assert(Method != Concrete->InterfaceMethods.end());
      auto Callee = mlir::LLVM::ExtractValueOp::create(
          Builder, Loc, Source,
          static_cast<std::size_t>(Method - Concrete->InterfaceMethods.begin() +
                                   1));
      Value = mlir::LLVM::InsertValueOp::create(
          Builder, Loc, Value, Callee,
          Builder.getDenseI64ArrayAttr({static_cast<int64_t>(I + 1)}));
      continue;
    }
    const sema::ClassInfo *MethodOwner = Concrete;
    const lex::Node *Method = nullptr;
    while (MethodOwner && !Method) {
      for (const auto &Member : MethodOwner->Node->children)
        if (Member->kind == K::ast_function && Member->text == Name) {
          Method = Member.get();
          break;
        }
      if (!Method)
        MethodOwner = MethodOwner->BaseName.empty()
                          ? nullptr
                          : Analysis.GetClass(MethodOwner->BaseName);
    }
    assert(Method && "interface conformance requires a method");
    mlir::Value Callee;
    for (const sema::ClassInfo *Owner = Concrete; Owner;
         Owner = Owner->BaseName.empty() ? nullptr
                                         : Analysis.GetClass(Owner->BaseName)) {
      const auto Slot = Owner->VirtualSlots.find(Name);
      if (Slot != Owner->VirtualSlots.end()) {
        auto Address = FieldAddress(*Owner, Object, Slot->second, Loc);
        Callee = mlir::LLVM::LoadOp::create(Builder, Loc, Pointer, Address);
        break;
      }
    }
    if (!Callee) {
      llvm::SmallVector<mlir::Type> Parameters{Pointer};
      llvm::SmallVector<mlir::Type> Results;
      for (const auto &Part : Method->children) {
        if (Part->kind == K::ast_parameter)
          Parameters.push_back(GetType(Analysis.GetType(*Part)));
        else if ((Part->kind == K::ast_type ||
                  Part->kind == K::ast_pointer_type ||
                  Part->kind == K::ast_array_type ||
                  Part->kind == K::ast_slice_type ||
                  Part->kind == K::ast_function_type) &&
                 !Analysis.GetType(*Part).IsVoid())
          Results.push_back(GetType(Analysis.GetType(*Part)));
      }
      auto Function = mlir::func::ConstantOp::create(
          Builder, Loc, Builder.getFunctionType(Parameters, Results),
          mlir::FlatSymbolRefAttr::get(&Context, Analysis.GetSymbol(*Method)));
      Callee = mlir::UnrealizedConversionCastOp::create(Builder, Loc, Pointer,
                                                        Function.getResult())
                   .getResult(0);
    }
    Value = mlir::LLVM::InsertValueOp::create(
        Builder, Loc, Value, Callee,
        Builder.getDenseI64ArrayAttr({static_cast<int64_t>(I + 1)}));
  }
  return Value;
}

mlir::Value codegen::IRGen::EmitNameExpression(const lex::Node &Expression) {
  if (Expression.text == "super")
    return FindVariable("this")->DirectValue;
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type = GetType(SemanticType);
  if (Analysis.GetField(Expression) || Analysis.GetStaticField(Expression))
    return mlir::LLVM::LoadOp::create(Builder, Loc, Type,
                                      EmitAddress(Expression));
  auto *Variable = FindVariable(Expression.text);
  if (!Variable->Address)
    return Variable->DirectValue;
  return mlir::LLVM::LoadOp::create(Builder, Loc, Type,
                                    EmitAddress(Expression));
}

mlir::Value codegen::IRGen::EmitIndexExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  if (Expression.kind == K::ast_member && Expression.text == "len") {
    const auto &Base = *Expression.children.front();
    const auto &BaseType = Analysis.GetType(Base);
    if (BaseType.IsArray())
      return mlir::arith::ConstantIntOp::create(Builder, Loc,
                                                BaseType.ArrayLength(), 64);
    if (BaseType.IsSlice()) {
      auto Slice = EmitExpression(Base);
      return mlir::LLVM::ExtractValueOp::create(Builder, Loc, Slice, 1);
    }
  }
  if (Expression.kind == K::ast_member && Analysis.GetField(Expression)) {
    const auto &Base = *Expression.children.front();
    if (Analysis.IsClassTemporary(Base)) {
      auto [Address, Temporary] = EmitClassSourceAddress(Base);
      const auto *Class = Analysis.GetClass(Analysis.GetType(Base));
      auto Field = FieldAddress(*Analysis.GetFieldOwner(Expression), Address,
                                Analysis.GetFieldIndex(Expression), Loc);
      auto Value = mlir::LLVM::LoadOp::create(
          Builder, Loc, GetType(Analysis.GetType(Expression)), Field);
      if (Temporary)
        mlir::func::CallOp::create(Builder, Loc, Class->DestructorSymbol,
                                   mlir::TypeRange{},
                                   mlir::ValueRange{Address});
      return Value;
    }
  }
  return mlir::LLVM::LoadOp::create(Builder, Loc,
                                    GetType(Analysis.GetType(Expression)),
                                    EmitAddress(Expression));
}

mlir::Value codegen::IRGen::EmitSliceExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  auto [Data, Length] = EmitSliceParts(*Expression.children.front());
  auto Zero = mlir::arith::ConstantIntOp::create(Builder, Loc, 0, 64);
  mlir::Value Start = Zero;
  mlir::Value End = Length;
  if (Expression.text == "start" || Expression.text == "both")
    Start = EmitSliceIndex(*Expression.children[1]);
  if (Expression.text == "end")
    End = EmitSliceIndex(*Expression.children[1]);
  else if (Expression.text == "both")
    End = EmitSliceIndex(*Expression.children[2]);
  auto Ordered = mlir::arith::CmpIOp::create(
      Builder, Loc, mlir::arith::CmpIPredicate::ule, Start, End);
  auto InRange = mlir::arith::CmpIOp::create(
      Builder, Loc, mlir::arith::CmpIPredicate::ule, End, Length);
  auto Valid = mlir::arith::AndIOp::create(Builder, Loc, Ordered, InRange);
  EmitSliceBoundsCheck(Valid, Loc);
  auto Pointer = mlir::LLVM::GEPOp::create(
      Builder, Loc, mlir::LLVM::LLVMPointerType::get(&Context),
      GetType(Analysis.GetType(Expression).Indexed()), Data,
      mlir::ValueRange{Start});
  auto Count = mlir::arith::SubIOp::create(Builder, Loc, End, Start);
  auto Result = mlir::LLVM::ZeroOp::create(
      Builder, Loc, GetType(Analysis.GetType(Expression)));
  auto WithPointer = mlir::LLVM::InsertValueOp::create(
      Builder, Loc, Result, Pointer, Builder.getDenseI64ArrayAttr({0}));
  return mlir::LLVM::InsertValueOp::create(Builder, Loc, WithPointer, Count,
                                           Builder.getDenseI64ArrayAttr({1}));
}

mlir::Value codegen::IRGen::EmitCallExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  if (Analysis.GetBaseConstructorCall(Expression)) {
    if (Analysis.GetInlineConstructor(Expression)) {
      EmitConstruction(Expression, FindVariable("this")->DirectValue);
      return {};
    }
    llvm::SmallVector<mlir::Value> Args{FindVariable("this")->DirectValue};
    for (std::size_t I = 1; I < Expression.children.size(); ++I) {
      const auto &Argument = *Expression.children[I];
      Args.push_back(Analysis.GetType(Argument).IsClass()
                         ? EmitClassArgument(Argument)
                         : EmitExpression(Argument));
    }
    mlir::func::CallOp::create(Builder, Loc, Analysis.GetCallee(Expression),
                               mlir::TypeRange{}, Args);
    return {};
  }
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type =
      SemanticType.IsVoid() ? mlir::Type() : GetType(SemanticType);
  llvm::SmallVector<mlir::Value> Arguments;
  mlir::Value ReceiverTemporary;
  if (Analysis.IsIndirectCall(Expression)) {
    const auto &Callee = *Expression.children.front();
    const auto &Signature = Analysis.GetType(Callee);
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : Signature.Parameters)
      Parameters.push_back(GetType(Parameter));
    Arguments.push_back(EmitExpression(Callee));
    for (std::size_t I = 1; I < Expression.children.size(); ++I)
      Arguments.push_back(EmitExpression(*Expression.children[I]));
    auto FunctionType = mlir::LLVM::LLVMFunctionType::get(
        Type ? Type : mlir::LLVM::LLVMVoidType::get(&Context), Parameters);
    auto Call =
        mlir::LLVM::CallOp::create(Builder, Loc, FunctionType, Arguments);
    return Type ? Call.getResult() : mlir::Value();
  }
  if (Analysis.IsMethodCall(Expression)) {
    const auto &Callee = *Expression.children.front();
    if (Callee.kind == K::ast_member) {
      const auto &Base = *Callee.children.front();
      if (Analysis.IsClassTemporary(Base)) {
        auto [Address, Temporary] = EmitClassSourceAddress(Base);
        Arguments.push_back(Address);
        if (Temporary)
          ReceiverTemporary = Address;
      } else {
        Arguments.push_back(Analysis.GetType(Base).IsPointer()
                                ? EmitExpression(Base)
                                : EmitAddress(Base));
      }
    } else {
      Arguments.push_back(FindVariable("this")->DirectValue);
    }
  }
  const auto *Wrapper = Analysis.GetCWrapper(Expression);
  mlir::Value ResultAddress;
  if (Wrapper && Wrapper->ReturnByAddress) {
    ResultAddress = CreateAlloca(SemanticType, Loc);
    Arguments.push_back(ResultAddress);
  }
  for (std::size_t I = 1; I < Expression.children.size(); ++I) {
    const auto &Argument = *Expression.children[I];
    if (Wrapper && Wrapper->ParametersByAddress[I - 1]) {
      if (Argument.kind == K::ast_name || Argument.kind == K::ast_index ||
          Argument.kind == K::ast_group) {
        Arguments.push_back(EmitAddress(Argument));
      } else {
        const auto &ArgumentType = Analysis.GetType(Argument);
        auto Address = CreateAlloca(ArgumentType, GetLocation(Argument.Loc));
        mlir::LLVM::StoreOp::create(Builder, GetLocation(Argument.Loc),
                                    EmitExpression(Argument), Address);
        Arguments.push_back(Address);
      }
    } else {
      Arguments.push_back(Analysis.GetType(Argument).IsClass()
                              ? EmitClassArgument(Argument)
                              : EmitExpression(Argument));
    }
  }
  const auto Callee = Wrapper ? Wrapper->Name : Analysis.GetCallee(Expression);
  if (Wrapper && Wrapper->ReturnByAddress) {
    mlir::func::CallOp::create(Builder, Loc, Callee, mlir::TypeRange{},
                               Arguments);
    return mlir::LLVM::LoadOp::create(Builder, Loc, Type, ResultAddress);
  }
  if (const auto *Interface = Analysis.GetInterfaceCall(Expression)) {
    auto Receiver = Arguments.front();
    Arguments.front() =
        mlir::LLVM::ExtractValueOp::create(Builder, Loc, Receiver, 0);
    auto Target = mlir::LLVM::ExtractValueOp::create(Builder, Loc, Receiver,
                                                     Interface->second + 1);
    llvm::SmallVector<mlir::Type> Parameters;
    for (auto Argument : Arguments)
      Parameters.push_back(Argument.getType());
    auto FunctionType = mlir::LLVM::LLVMFunctionType::get(
        Type ? Type : mlir::LLVM::LLVMVoidType::get(&Context), Parameters);
    llvm::SmallVector<mlir::Value> IndirectArguments{Target};
    IndirectArguments.append(Arguments.begin(), Arguments.end());
    auto Call = mlir::LLVM::CallOp::create(Builder, Loc, FunctionType,
                                           IndirectArguments);
    return Type ? Call.getResult() : mlir::Value();
  }
  if (const auto *Virtual = Analysis.GetVirtualCall(Expression)) {
    const auto &Owner = *Analysis.GetClass(Virtual->first);
    const auto &Signature = Owner.Fields[Virtual->second].Value;
    auto Slot = FieldAddress(Owner, Arguments.front(), Virtual->second, Loc);
    auto Callee =
        mlir::LLVM::LoadOp::create(Builder, Loc, GetType(Signature), Slot);
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : Signature.Parameters)
      Parameters.push_back(GetType(Parameter));
    auto FunctionType = mlir::LLVM::LLVMFunctionType::get(
        Type ? Type : mlir::LLVM::LLVMVoidType::get(&Context), Parameters);
    llvm::SmallVector<mlir::Value> IndirectArguments{Callee.getResult()};
    IndirectArguments.append(Arguments.begin(), Arguments.end());
    auto Call = mlir::LLVM::CallOp::create(Builder, Loc, FunctionType,
                                           IndirectArguments);
    if (ReceiverTemporary) {
      const auto &Base = *Expression.children.front()->children.front();
      mlir::func::CallOp::create(
          Builder, Loc,
          Analysis.GetClass(Analysis.GetType(Base))->DestructorSymbol,
          mlir::TypeRange{}, mlir::ValueRange{ReceiverTemporary});
    }
    return Type ? Call.getResult() : mlir::Value();
  }
  llvm::SmallVector<mlir::Type> Results;
  if (Type)
    Results.push_back(Type);
  auto Call =
      mlir::func::CallOp::create(Builder, Loc, Callee, Results, Arguments);
  if (ReceiverTemporary) {
    const auto &Base = *Expression.children.front()->children.front();
    mlir::func::CallOp::create(
        Builder, Loc,
        Analysis.GetClass(Analysis.GetType(Base))->DestructorSymbol,
        mlir::TypeRange{}, mlir::ValueRange{ReceiverTemporary});
  }
  return Type ? Call.getResult(0) : mlir::Value();
}

mlir::Value codegen::IRGen::EmitLiteralExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type = GetType(SemanticType);
  if (!Expression.text.empty() && Expression.text.front() == '"') {
    std::string Value;
    for (std::size_t I = 1; I + 1 < Expression.text.size(); ++I) {
      if (Expression.text[I] != '\\') {
        Value.push_back(Expression.text[I]);
        continue;
      }
      const char Escaped = Expression.text[++I];
      Value.push_back(Escaped == 'n'   ? '\n'
                      : Escaped == 'r' ? '\r'
                      : Escaped == 't' ? '\t'
                      : Escaped == '0' ? '\0'
                                       : Escaped);
    }
    Value.push_back('\0');
    return mlir::LLVM::createGlobalString(
        Loc, Builder, "kelyra_string_" + std::to_string(GlobalStringCount++),
        Value, mlir::LLVM::Linkage::Private);
  }
  if (SemanticType.Element == sema::BuiltinType::Bool) {
    llvm::APInt Value(1, Expression.text == "true");
    return mlir::arith::ConstantIntOp::create(Builder, Loc, Type, Value);
  }
  if (sema::IsInteger(SemanticType.Element) ||
      SemanticType.Element == sema::BuiltinType::Char) {
    llvm::APInt Value;
    llvm::StringRef(Expression.text).getAsInteger(10, Value);
    Value = Value.zextOrTrunc(sema::GetBitWidth(SemanticType));
    return mlir::arith::ConstantIntOp::create(Builder, Loc, Type, Value);
  }
  const auto FloatType = mlir::cast<mlir::FloatType>(Type);
  llvm::APFloat Value(FloatType.getFloatSemantics(), Expression.text);
  return mlir::arith::ConstantFloatOp::create(Builder, Loc, FloatType, Value);
}

mlir::Value codegen::IRGen::EmitGroupExpression(const lex::Node &Expression) {
  return EmitExpression(*Expression.children.front());
}

mlir::Value codegen::IRGen::EmitUnaryExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type = GetType(SemanticType);
  if (Expression.text == "&")
    return EmitAddress(*Expression.children.front());
  if (Expression.text == "*")
    return mlir::LLVM::LoadOp::create(
        Builder, Loc, Type, EmitExpression(*Expression.children.front()));
  auto Value = EmitExpression(*Expression.children.front());
  if (Expression.text == "+")
    return Value;
  if (Expression.text == "!") {
    auto True = mlir::arith::ConstantIntOp::create(Builder, Loc, 1, 1);
    return mlir::arith::XOrIOp::create(Builder, Loc, Value, True);
  }
  if (sema::IsFloat(SemanticType.Element))
    return mlir::arith::NegFOp::create(Builder, Loc, Value);
  auto Zero = mlir::arith::ConstantIntOp::create(
      Builder, Loc, Type, llvm::APInt(sema::GetBitWidth(SemanticType), 0));
  return mlir::arith::SubIOp::create(Builder, Loc, Zero, Value);
}

mlir::Value codegen::IRGen::EmitBlockExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  Scopes.emplace_back();
  Cleanups.emplace_back();
  mlir::Value Result;
  for (std::size_t I = 0; I < Expression.children.size(); ++I) {
    if (!Builder.getInsertionBlock()->empty() &&
        Builder.getInsertionBlock()
            ->back()
            .hasTrait<mlir::OpTrait::IsTerminator>())
      break;
    const auto &Child = *Expression.children[I];
    const bool Tail = I + 1 == Expression.children.size() &&
                      lex::IsExpressionNode(Child.kind);
    if (Tail)
      Result = EmitExpression(Child);
    else
      EmitStatement(Child);
  }
  if (Builder.getInsertionBlock()->empty() ||
      !Builder.getInsertionBlock()
           ->back()
           .hasTrait<mlir::OpTrait::IsTerminator>())
    EmitCleanups(Cleanups.size() - 1, Loc);
  Cleanups.pop_back();
  Scopes.pop_back();
  return Result;
}

mlir::Value codegen::IRGen::EmitMatchExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  auto Scrutinee = EmitExpression(*Expression.children.front());
  const auto &ResultType = Analysis.GetType(Expression);
  mlir::Value ResultAddress;
  if (!ResultType.IsVoid())
    ResultAddress = CreateAlloca(ResultType, Loc);
  auto *Region = Builder.getInsertionBlock()->getParent();
  auto *Join = new mlir::Block();
  Region->push_back(Join);
  for (std::size_t I = 1; I < Expression.children.size(); ++I) {
    const auto &Arm = *Expression.children[I];
    auto *Body = new mlir::Block();
    Region->push_back(Body);
    const auto *Pattern = Analysis.GetMatchPatternValue(*Arm.children.front());
    const bool Last = I + 1 == Expression.children.size();
    mlir::Block *Next = nullptr;
    if (!Pattern || Last) {
      mlir::cf::BranchOp::create(Builder, Loc, Body);
    } else {
      Next = new mlir::Block();
      Region->push_back(Next);
      const bool Negative = Pattern->starts_with('-');
      llvm::StringRef Digits(Pattern->data() + (Negative ? 1 : 0),
                             Pattern->size() - (Negative ? 1 : 0));
      auto Integer = llvm::APInt(
          sema::GetBitWidth(Analysis.GetType(*Expression.children.front())),
          Digits, 10);
      if (Negative)
        Integer = -Integer;
      auto Constant = mlir::arith::ConstantIntOp::create(
          Builder, Loc, Scrutinee.getType(), Integer);
      auto Equal = mlir::arith::CmpIOp::create(
          Builder, Loc, mlir::arith::CmpIPredicate::eq, Scrutinee, Constant);
      mlir::cf::CondBranchOp::create(Builder, Loc, Equal, Body, Next);
    }
    Builder.setInsertionPointToStart(Body);
    auto Value = EmitExpression(*Arm.children.back());
    if (Builder.getInsertionBlock()->empty() ||
        !Builder.getInsertionBlock()
             ->back()
             .hasTrait<mlir::OpTrait::IsTerminator>()) {
      if (ResultAddress)
        mlir::LLVM::StoreOp::create(Builder, Loc, Value, ResultAddress);
      mlir::cf::BranchOp::create(Builder, Loc, Join);
    }
    if (Next)
      Builder.setInsertionPointToStart(Next);
  }
  Builder.setInsertionPointToStart(Join);
  return ResultAddress ? mlir::LLVM::LoadOp::create(
                             Builder, Loc, GetType(ResultType), ResultAddress)
                             .getResult()
                       : mlir::Value();
}

mlir::Value codegen::IRGen::EmitCastExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &Source = Analysis.GetType(*Expression.children.front());
  const auto &Target = Analysis.GetType(Expression);
  auto Value = EmitExpression(*Expression.children.front());
  const bool SourceAddress = Source.IsPointer() || Source.IsFunction();
  const bool TargetAddress = Target.IsPointer() || Target.IsFunction();
  if (Source == Target || (SourceAddress && TargetAddress))
    return Value;
  const auto ResultType = GetType(Target);
  if (SourceAddress)
    return mlir::LLVM::PtrToIntOp::create(Builder, Loc, ResultType, Value);
  if (TargetAddress)
    return mlir::LLVM::IntToPtrOp::create(Builder, Loc, ResultType, Value);
  const bool SourceFloat = sema::IsFloat(Source.Element);
  const bool TargetFloat = sema::IsFloat(Target.Element);
  if (SourceFloat && TargetFloat) {
    if (sema::GetBitWidth(Source) == sema::GetBitWidth(Target))
      return Value;
    if (sema::GetBitWidth(Source) < sema::GetBitWidth(Target))
      return mlir::arith::ExtFOp::create(Builder, Loc, ResultType, Value,
                                         mlir::arith::FastMathFlagsAttr{});
    return mlir::arith::TruncFOp::create(Builder, Loc, ResultType, Value);
  }
  if (SourceFloat) {
    if (sema::IsSignedInteger(Target.Element))
      return mlir::arith::FPToSIOp::create(Builder, Loc, ResultType, Value);
    return mlir::arith::FPToUIOp::create(Builder, Loc, ResultType, Value);
  }
  if (TargetFloat) {
    if (sema::IsSignedInteger(Source.Element))
      return mlir::arith::SIToFPOp::create(Builder, Loc, ResultType, Value);
    return mlir::arith::UIToFPOp::create(Builder, Loc, ResultType, Value);
  }
  if (sema::GetBitWidth(Source) < sema::GetBitWidth(Target)) {
    const auto *Enum =
        Source.IsEnum() ? Analysis.GetEnum(Source.EnumName) : nullptr;
    if (sema::IsSignedInteger(Enum ? Enum->Underlying.Element : Source.Element))
      return mlir::arith::ExtSIOp::create(Builder, Loc, ResultType, Value);
    return mlir::arith::ExtUIOp::create(Builder, Loc, ResultType, Value);
  }
  if (sema::GetBitWidth(Source) > sema::GetBitWidth(Target))
    return mlir::arith::TruncIOp::create(Builder, Loc, ResultType, Value);
  return Value;
}

mlir::Value codegen::IRGen::EmitBinaryExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type = GetType(SemanticType);
  auto Lhs = EmitExpression(*Expression.children[0]);
  auto Rhs = EmitExpression(*Expression.children[1]);
  const auto &OperandType = Analysis.GetType(*Expression.children[0]);
  if (Expression.text == "&&")
    return mlir::arith::AndIOp::create(Builder, Loc, Lhs, Rhs);
  if (Expression.text == "||")
    return mlir::arith::OrIOp::create(Builder, Loc, Lhs, Rhs);
  const bool Comparison = Expression.text == "==" || Expression.text == "!=" ||
                          Expression.text == "<" || Expression.text == "<=" ||
                          Expression.text == ">" || Expression.text == ">=";
  if (Comparison) {
    if (sema::IsFloat(OperandType.Element)) {
      auto Predicate = mlir::arith::CmpFPredicate::OEQ;
      if (Expression.text == "!=")
        Predicate = mlir::arith::CmpFPredicate::ONE;
      else if (Expression.text == "<")
        Predicate = mlir::arith::CmpFPredicate::OLT;
      else if (Expression.text == "<=")
        Predicate = mlir::arith::CmpFPredicate::OLE;
      else if (Expression.text == ">")
        Predicate = mlir::arith::CmpFPredicate::OGT;
      else if (Expression.text == ">=")
        Predicate = mlir::arith::CmpFPredicate::OGE;
      return mlir::arith::CmpFOp::create(Builder, Loc, Predicate, Lhs, Rhs);
    }
    auto Predicate = mlir::arith::CmpIPredicate::eq;
    if (Expression.text == "!=")
      Predicate = mlir::arith::CmpIPredicate::ne;
    else if (Expression.text == "<")
      Predicate = sema::IsSignedInteger(OperandType.Element)
                      ? mlir::arith::CmpIPredicate::slt
                      : mlir::arith::CmpIPredicate::ult;
    else if (Expression.text == "<=")
      Predicate = sema::IsSignedInteger(OperandType.Element)
                      ? mlir::arith::CmpIPredicate::sle
                      : mlir::arith::CmpIPredicate::ule;
    else if (Expression.text == ">")
      Predicate = sema::IsSignedInteger(OperandType.Element)
                      ? mlir::arith::CmpIPredicate::sgt
                      : mlir::arith::CmpIPredicate::ugt;
    else if (Expression.text == ">=")
      Predicate = sema::IsSignedInteger(OperandType.Element)
                      ? mlir::arith::CmpIPredicate::sge
                      : mlir::arith::CmpIPredicate::uge;
    return mlir::arith::CmpIOp::create(Builder, Loc, Predicate, Lhs, Rhs);
  }
  if (sema::IsFloat(SemanticType.Element)) {
    if (Expression.text == "+")
      return mlir::arith::AddFOp::create(Builder, Loc, Lhs, Rhs);
    if (Expression.text == "-")
      return mlir::arith::SubFOp::create(Builder, Loc, Lhs, Rhs);
    if (Expression.text == "*")
      return mlir::arith::MulFOp::create(Builder, Loc, Lhs, Rhs);
    return mlir::arith::DivFOp::create(Builder, Loc, Lhs, Rhs);
  }
  if (Expression.text == "+")
    return mlir::arith::AddIOp::create(Builder, Loc, Lhs, Rhs);
  if (Expression.text == "-")
    return mlir::arith::SubIOp::create(Builder, Loc, Lhs, Rhs);
  if (Expression.text == "*")
    return mlir::arith::MulIOp::create(Builder, Loc, Lhs, Rhs);
  if (Expression.text == "/") {
    if (sema::IsSignedInteger(SemanticType.Element))
      return mlir::arith::DivSIOp::create(Builder, Loc, Lhs, Rhs);
    return mlir::arith::DivUIOp::create(Builder, Loc, Lhs, Rhs);
  }
  if (sema::IsSignedInteger(SemanticType.Element))
    return mlir::arith::RemSIOp::create(Builder, Loc, Lhs, Rhs);
  return mlir::arith::RemUIOp::create(Builder, Loc, Lhs, Rhs);
}
