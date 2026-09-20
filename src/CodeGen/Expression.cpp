#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cassert>
#include <unordered_map>

using namespace kelyra;
using K = lex::TokenKind;

mlir::Value codegen::IRGen::EmitExpression(const lex::Node &Expression) {
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
      {K::ast_member, &IRGen::EmitIndexExpression},
      {K::ast_call, &IRGen::EmitCallExpression},
      {K::ast_literal, &IRGen::EmitLiteralExpression},
      {K::ast_group, &IRGen::EmitGroupExpression},
      {K::ast_unary, &IRGen::EmitUnaryExpression},
      {K::ast_binary, &IRGen::EmitBinaryExpression},
  };
  const auto It = Handlers.find(Expression.kind);
  assert(It != Handlers.end() &&
         "semantic analysis accepted an expression without an IR handler");
  return (this->*It->second)(Expression);
}

mlir::Value codegen::IRGen::EmitNameExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type = GetType(SemanticType);
  if (Analysis.GetField(Expression))
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
  return mlir::LLVM::LoadOp::create(Builder, Loc,
                                    GetType(Analysis.GetType(Expression)),
                                    EmitAddress(Expression));
}

mlir::Value codegen::IRGen::EmitCallExpression(const lex::Node &Expression) {
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type =
      SemanticType.IsVoid() ? mlir::Type() : GetType(SemanticType);
  llvm::SmallVector<mlir::Value> Arguments;
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
      Arguments.push_back(Analysis.GetType(Base).IsPointer()
                              ? EmitExpression(Base)
                              : EmitAddress(Base));
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
      Arguments.push_back(EmitExpression(Argument));
    }
  }
  const auto Callee = Wrapper ? Wrapper->Name : Analysis.GetCallee(Expression);
  if (Wrapper && Wrapper->ReturnByAddress) {
    mlir::func::CallOp::create(Builder, Loc, Callee, mlir::TypeRange{},
                               Arguments);
    return mlir::LLVM::LoadOp::create(Builder, Loc, Type, ResultAddress);
  }
  llvm::SmallVector<mlir::Type> Results;
  if (Type)
    Results.push_back(Type);
  auto Call =
      mlir::func::CallOp::create(Builder, Loc, Callee, Results, Arguments);
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
