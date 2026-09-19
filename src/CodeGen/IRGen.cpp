#include "CodeGen/IRGen.h"
#include "IR/Kelyra.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/Transforms/Passes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <system_error>

using namespace kelyra;

namespace {
bool IsTypeNode(lex::TokenKind Kind) {
  using K = lex::TokenKind;
  return Kind == K::ast_type || Kind == K::ast_pointer_type ||
         Kind == K::ast_array_type;
}

bool HasTerminator(mlir::Block *Block) {
  return !Block->empty() &&
         Block->back().hasTrait<mlir::OpTrait::IsTerminator>();
}

llvm::OptimizationLevel GetOptimizationLevel(unsigned Level) {
  switch (Level) {
  case 1:
    return llvm::OptimizationLevel::O1;
  case 2:
    return llvm::OptimizationLevel::O2;
  case 3:
    return llvm::OptimizationLevel::O3;
  default:
    return llvm::OptimizationLevel::O0;
  }
}

void Optimize(llvm::Module &Module, llvm::TargetMachine &TargetMachine,
              llvm::OptimizationLevel Level) {
  if (Level == llvm::OptimizationLevel::O0)
    return;
  llvm::LoopAnalysisManager LoopAnalyses;
  llvm::FunctionAnalysisManager FunctionAnalyses;
  llvm::CGSCCAnalysisManager CGSCCAnalyses;
  llvm::ModuleAnalysisManager ModuleAnalyses;
  llvm::PassBuilder Builder(&TargetMachine);
  Builder.registerLoopAnalyses(LoopAnalyses);
  Builder.registerFunctionAnalyses(FunctionAnalyses);
  Builder.registerCGSCCAnalyses(CGSCCAnalyses);
  Builder.registerModuleAnalyses(ModuleAnalyses);
  Builder.crossRegisterProxies(LoopAnalyses, FunctionAnalyses, CGSCCAnalyses,
                               ModuleAnalyses);
  auto Passes = Builder.buildPerModuleDefaultPipeline(Level);
  Passes.run(Module, ModuleAnalyses);
}
} // namespace

codegen::IRGen::IRGen(mlir::MLIRContext &Context, const sema::Sema &Analysis,
                      unsigned SafeLevel)
    : Context(Context), Analysis(Analysis), SafeLevel(SafeLevel),
      Builder(&Context) {
  Context.loadDialect<ir::KelyraDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect>();
}

mlir::Location codegen::IRGen::GetLocation(const lex::Location &Loc) {
  if (Loc.File.empty())
    return Builder.getUnknownLoc();
  return mlir::FileLineColLoc::get(
      &Context, llvm::StringRef(Loc.File.data(), Loc.File.size()), Loc.Line,
      Loc.Column);
}

mlir::Type codegen::IRGen::GetType(const sema::Type &Type) {
  using T = sema::BuiltinType;
  if (Type.IsPointer())
    return mlir::LLVM::LLVMPointerType::get(&Context);
  if (Type.IsRecord())
    return mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(),
                                          (sema::GetBitWidth(Type) + 7) / 8);
  mlir::Type Result;
  if (sema::IsInteger(Type.Element))
    Result = Builder.getIntegerType(sema::GetBitWidth(Type));
  else {
    switch (Type.Element) {
    case T::F32:
      Result = Builder.getF32Type();
      break;
    case T::F64:
      Result = Builder.getF64Type();
      break;
    case T::F128:
      Result = Builder.getF128Type();
      break;
    case T::F256:
      Result = ir::F256Type::get(&Context);
      break;
    case T::F512:
      Result = ir::F512Type::get(&Context);
      break;
    case T::Bool:
      Result = Builder.getI1Type();
      break;
    case T::CBool:
      Result = Builder.getIntegerType(sema::GetBitWidth(Type));
      break;
    case T::Char:
      Result = Builder.getI32Type();
      break;
    default:
      llvm_unreachable("unhandled builtin type");
    }
  }
  for (auto Dimension = Type.Dimensions.rbegin();
       Dimension != Type.Dimensions.rend(); ++Dimension)
    Result = mlir::LLVM::LLVMArrayType::get(Result, *Dimension);
  return Result;
}

codegen::IRGen::Variable *codegen::IRGen::FindVariable(std::string_view Name) {
  for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope) {
    const auto It = Scope->find(std::string(Name));
    if (It != Scope->end())
      return &It->second;
  }
  llvm_unreachable("semantic analysis accepted an unknown variable");
}

mlir::Value codegen::IRGen::CreateAlloca(const sema::Type &Type,
                                         mlir::Location Loc) {
  mlir::OpBuilder::InsertionGuard Guard(Builder);
  Builder.setInsertionPointToStart(FunctionEntry);
  auto One = mlir::arith::ConstantIntOp::create(Builder, Loc, 1, 64);
  return mlir::LLVM::AllocaOp::create(
      Builder, Loc, mlir::LLVM::LLVMPointerType::get(&Context), GetType(Type),
      One, Type.Alignment);
}

mlir::Value codegen::IRGen::EmitAddress(const lex::Node &Expression) {
  using K = lex::TokenKind;
  if (Expression.kind == K::ast_name)
    return FindVariable(Expression.text)->Address;
  if (Expression.kind == K::ast_group)
    return EmitAddress(*Expression.children.front());

  const auto &Base = *Expression.children[0];
  const auto BaseType = Analysis.GetType(Base);
  auto BaseAddress = EmitAddress(Base);
  const auto &IndexExpression = *Expression.children[1];
  const auto &IndexType = Analysis.GetType(IndexExpression);
  auto Index = EmitExpression(IndexExpression);
  if (SafeLevel > 0) {
    const auto BitWidth = sema::GetBitWidth(IndexType);
    mlir::Value Valid;
    if (sema::IsSignedInteger(IndexType.Element)) {
      auto Zero = mlir::arith::ConstantIntOp::create(
          Builder, GetLocation(IndexExpression.Loc), GetType(IndexType),
          llvm::APInt(BitWidth, 0));
      Valid = mlir::arith::CmpIOp::create(
          Builder, GetLocation(IndexExpression.Loc),
          mlir::arith::CmpIPredicate::sge, Index, Zero);
    }
    const auto Length = BaseType.Dimensions.front();
    const bool LengthFits =
        BitWidth >= 64 || Length < (std::uint64_t{1} << BitWidth);
    if (LengthFits) {
      auto Limit = mlir::arith::ConstantIntOp::create(
          Builder, GetLocation(IndexExpression.Loc), GetType(IndexType),
          llvm::APInt(BitWidth, Length));
      auto BelowLimit = mlir::arith::CmpIOp::create(
          Builder, GetLocation(IndexExpression.Loc),
          mlir::arith::CmpIPredicate::ult, Index, Limit);
      if (Valid)
        Valid = mlir::arith::AndIOp::create(
            Builder, GetLocation(IndexExpression.Loc), Valid, BelowLimit);
      else
        Valid = BelowLimit;
    }
    if (Valid)
      mlir::cf::AssertOp::create(Builder, GetLocation(IndexExpression.Loc),
                                 Valid, "array index out of bounds");
  }
  auto Zero = mlir::arith::ConstantIntOp::create(
      Builder, GetLocation(Expression.Loc), 0, 32);
  llvm::SmallVector<mlir::Value> Indices{Zero, Index};
  return mlir::LLVM::GEPOp::create(Builder, GetLocation(Expression.Loc),
                                   mlir::LLVM::LLVMPointerType::get(&Context),
                                   GetType(BaseType), BaseAddress, Indices);
}

mlir::Value codegen::IRGen::EmitExpression(const lex::Node &Expression) {
  using K = lex::TokenKind;
  const auto Loc = GetLocation(Expression.Loc);
  const auto &SemanticType = Analysis.GetType(Expression);
  const auto Type = GetType(SemanticType);
  if (Expression.kind == K::ast_name) {
    auto *Variable = FindVariable(Expression.text);
    if (!Variable->Address)
      return Variable->DirectValue;
    return mlir::LLVM::LoadOp::create(Builder, Loc, Type,
                                      EmitAddress(Expression));
  }
  if (Expression.kind == K::ast_index)
    return mlir::LLVM::LoadOp::create(Builder, Loc, Type,
                                      EmitAddress(Expression));
  if (Expression.kind == K::ast_call) {
    llvm::SmallVector<mlir::Value> Arguments;
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
    const auto Callee =
        Wrapper ? Wrapper->Name : Analysis.GetCallee(Expression);
    if (Wrapper && Wrapper->ReturnByAddress) {
      mlir::func::CallOp::create(Builder, Loc, Callee, mlir::TypeRange{},
                                 Arguments);
      return mlir::LLVM::LoadOp::create(Builder, Loc, Type, ResultAddress);
    }
    return mlir::func::CallOp::create(Builder, Loc, Callee,
                                      mlir::TypeRange{Type}, Arguments)
        .getResult(0);
  }
  if (Expression.kind == K::ast_literal) {
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
  if (Expression.kind == K::ast_group)
    return EmitExpression(*Expression.children.front());
  if (Expression.kind == K::ast_unary) {
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

void codegen::IRGen::EmitBlock(const lex::Node &Block) {
  Scopes.emplace_back();
  for (const auto &Statement : Block.children) {
    if (HasTerminator(Builder.getInsertionBlock()))
      break;
    EmitStatement(*Statement);
  }
  Scopes.pop_back();
}

void codegen::IRGen::EmitStatement(const lex::Node &Statement) {
  using K = lex::TokenKind;
  const auto Loc = GetLocation(Statement.Loc);
  if (Statement.kind == K::ast_block) {
    EmitBlock(Statement);
    return;
  }
  if (Statement.kind == K::ast_let) {
    const auto &Name = *Statement.children[0];
    const auto Type = Analysis.GetType(Statement);
    auto Address = CreateAlloca(Type, Loc);
    const bool HasType = Statement.children.size() > 1 &&
                         IsTypeNode(Statement.children[1]->kind);
    const lex::Node *Initializer =
        Statement.children.size() > (HasType ? 2u : 1u)
            ? Statement.children.back().get()
            : nullptr;
    auto Value =
        Initializer
            ? EmitExpression(*Initializer)
            : mlir::LLVM::ZeroOp::create(Builder, Loc, GetType(Type)).getRes();
    mlir::LLVM::StoreOp::create(Builder, Loc, Value, Address);
    Scopes.back().emplace(Name.text, Variable{Type, Address, {}});
    return;
  }
  if (Statement.kind == K::ast_assign) {
    auto Address = EmitAddress(*Statement.children[0]);
    auto Value = EmitExpression(*Statement.children[1]);
    mlir::LLVM::StoreOp::create(Builder, Loc, Value, Address);
    return;
  }
  if (Statement.kind == K::ast_expr_stmt) {
    EmitExpression(*Statement.children.front());
    return;
  }
  if (Statement.kind == K::ast_return) {
    mlir::func::ReturnOp::create(Builder, Loc,
                                 EmitExpression(*Statement.children.front()));
    return;
  }
  if (Statement.kind == K::ast_break) {
    mlir::cf::BranchOp::create(Builder, Loc, Loops.back().Break);
    return;
  }
  if (Statement.kind == K::ast_continue) {
    mlir::cf::BranchOp::create(Builder, Loc, Loops.back().Continue);
    return;
  }
  auto *Region = Builder.getInsertionBlock()->getParent();
  if (Statement.kind == K::ast_if) {
    auto Condition = EmitExpression(*Statement.children[0]);
    auto *Then = new mlir::Block();
    auto *Else = new mlir::Block();
    auto *After = new mlir::Block();
    Region->push_back(Then);
    Region->push_back(Else);
    Region->push_back(After);
    mlir::cf::CondBranchOp::create(Builder, Loc, Condition, Then, Else);

    Builder.setInsertionPointToStart(Then);
    EmitBlock(*Statement.children[1]);
    if (!HasTerminator(Builder.getInsertionBlock()))
      mlir::cf::BranchOp::create(Builder, Loc, After);

    Builder.setInsertionPointToStart(Else);
    if (Statement.children.size() == 3) {
      if (Statement.children[2]->kind == K::ast_if)
        EmitStatement(*Statement.children[2]);
      else
        EmitBlock(*Statement.children[2]);
    }
    if (!HasTerminator(Builder.getInsertionBlock()))
      mlir::cf::BranchOp::create(Builder, Loc, After);
    Builder.setInsertionPointToStart(After);
    return;
  }
  if (Statement.kind == K::ast_while) {
    auto *Header = new mlir::Block();
    auto *Body = new mlir::Block();
    auto *After = new mlir::Block();
    Region->push_back(Header);
    Region->push_back(Body);
    Region->push_back(After);
    mlir::cf::BranchOp::create(Builder, Loc, Header);

    Builder.setInsertionPointToStart(Header);
    auto Condition = EmitExpression(*Statement.children[0]);
    mlir::cf::CondBranchOp::create(Builder, Loc, Condition, Body, After);

    Builder.setInsertionPointToStart(Body);
    Loops.push_back({After, Header});
    EmitBlock(*Statement.children[1]);
    Loops.pop_back();
    if (!HasTerminator(Builder.getInsertionBlock()))
      mlir::cf::BranchOp::create(Builder, Loc, Header);
    Builder.setInsertionPointToStart(After);
  }
}

void codegen::IRGen::EmitFunction(const lex::Node &Function) {
  using K = lex::TokenKind;
  llvm::SmallVector<const lex::Node *> Parameters;
  const lex::Node *ReturnType = nullptr;
  const lex::Node *Body = nullptr;
  for (const auto &Child : Function.children) {
    if (Child->kind == K::ast_parameter)
      Parameters.push_back(Child.get());
    else if (IsTypeNode(Child->kind))
      ReturnType = Child.get();
    else if (Child->kind == K::ast_block)
      Body = Child.get();
  }

  llvm::SmallVector<mlir::Type> ParameterTypes;
  for (const auto *Parameter : Parameters)
    ParameterTypes.push_back(GetType(Analysis.GetType(*Parameter)));
  auto FunctionType = Builder.getFunctionType(
      ParameterTypes, {GetType(Analysis.GetType(*ReturnType))});
  auto Func =
      mlir::func::FuncOp::create(Builder, GetLocation(Function.Loc),
                                 Analysis.GetSymbol(Function), FunctionType);
  if (!Analysis.IsPublic(Function) && Function.text != "main")
    Func.setPrivate();
  auto *Entry = Func.addEntryBlock();
  FunctionEntry = Entry;
  Builder.setInsertionPointToStart(Entry);
  Scopes.clear();
  Scopes.emplace_back();
  for (std::size_t I = 0; I < Parameters.size(); ++I) {
    const auto Type = Analysis.GetType(*Parameters[I]);
    if (!Type.IsRecord() && sema::GetBitWidth(Type) > 128) {
      Scopes.back().emplace(Parameters[I]->text,
                            Variable{Type, {}, Entry->getArgument(I)});
      continue;
    }
    auto Address = CreateAlloca(Type, GetLocation(Parameters[I]->Loc));
    mlir::LLVM::StoreOp::create(Builder, GetLocation(Parameters[I]->Loc),
                                Entry->getArgument(I), Address);
    Scopes.back().emplace(Parameters[I]->text, Variable{Type, Address, {}});
  }
  EmitBlock(*Body);
  if (!HasTerminator(Builder.getInsertionBlock()))
    mlir::LLVM::UnreachableOp::create(Builder, GetLocation(Body->Loc));
}

mlir::OwningOpRef<mlir::ModuleOp>
codegen::IRGen::Generate(const lex::Node &Module) {
  return Generate({&Module});
}

mlir::OwningOpRef<mlir::ModuleOp>
codegen::IRGen::Generate(llvm::ArrayRef<const lex::Node *> Modules) {
  using K = lex::TokenKind;
  if (Modules.empty())
    return {};
  auto Result = mlir::ModuleOp::create(GetLocation(Modules.front()->Loc));
  Builder.setInsertionPointToEnd(Result.getBody());
  for (const auto &External : Analysis.GetExternalFunctions()) {
    if (External.Variadic || External.Return.IsRecord() ||
        std::any_of(External.Parameters.begin(), External.Parameters.end(),
                    [](const sema::Type &Type) { return Type.IsRecord(); }))
      continue;
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : External.Parameters)
      Parameters.push_back(GetType(Parameter));
    auto Function = mlir::func::FuncOp::create(
        Builder, Builder.getUnknownLoc(), External.Name,
        Builder.getFunctionType(Parameters, {GetType(External.Return)}));
    Function.setPrivate();
  }
  for (const auto &Wrapper : Analysis.GetCWrappers()) {
    llvm::SmallVector<mlir::Type> Parameters;
    for (const auto &Parameter : Wrapper.Parameters)
      Parameters.push_back(GetType(Parameter));
    llvm::SmallVector<mlir::Type> Results;
    if (!Wrapper.ReturnByAddress)
      Results.push_back(GetType(Wrapper.Return));
    auto Function = mlir::func::FuncOp::create(
        Builder, Builder.getUnknownLoc(), Wrapper.Name,
        Builder.getFunctionType(Parameters, Results));
    Function.setPrivate();
  }
  for (const auto *Module : Modules) {
    for (const auto &Child : Module->children) {
      if (Child->kind != K::ast_function)
        continue;
      Builder.setInsertionPointToEnd(Result.getBody());
      EmitFunction(*Child);
    }
  }
  return Result;
}

llvm::Error codegen::EmitObject(mlir::ModuleOp Module,
                                llvm::StringRef OutputPath, unsigned OptLevel,
                                llvm::StringRef CWrapperSource,
                                llvm::ArrayRef<std::string> CArguments) {
  mlir::PassManager Passes(Module.getContext());
  Passes.addPass(mlir::createConvertFuncToLLVMPass());
  Passes.addPass(mlir::createArithToLLVMConversionPass());
  Passes.addPass(mlir::createConvertControlFlowToLLVMPass());
  if (OptLevel == 0) {
    mlir::LLVM::DIScopeForLLVMFuncOpPassOptions DebugOptions;
    DebugOptions.emissionKind = mlir::LLVM::DIEmissionKind::Full;
    Passes.addPass(mlir::LLVM::createDIScopeForLLVMFuncOpPass(DebugOptions));
  }
  if (mlir::failed(Passes.run(Module)))
    return llvm::createStringError("failed to lower MLIR to LLVM dialect");

  mlir::registerBuiltinDialectTranslation(*Module.getContext());
  mlir::registerLLVMDialectTranslation(*Module.getContext());
  llvm::LLVMContext Context;
  auto LLVMModule = mlir::translateModuleToLLVMIR(Module, Context);
  if (!LLVMModule)
    return llvm::createStringError("failed to translate MLIR to LLVM IR");

  if (llvm::InitializeNativeTarget() ||
      llvm::InitializeNativeTargetAsmPrinter())
    return llvm::createStringError("failed to initialize native target");

  const llvm::Triple Triple(llvm::sys::getDefaultTargetTriple());
  std::string TargetError;
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(Triple, TargetError);
  if (!Target)
    return llvm::createStringError(TargetError);

  llvm::TargetOptions Options;
  const auto CodeGenLevel = *llvm::CodeGenOpt::getLevel(OptLevel);
  std::unique_ptr<llvm::TargetMachine> TargetMachine(
      Target->createTargetMachine(Triple, llvm::sys::getHostCPUName(), "",
                                  Options, llvm::Reloc::PIC_, std::nullopt,
                                  CodeGenLevel));
  if (!TargetMachine)
    return llvm::createStringError("failed to create target machine");
  LLVMModule->setDataLayout(TargetMachine->createDataLayout());
  LLVMModule->setTargetTriple(Triple);
  const auto OptimizationLevel = GetOptimizationLevel(OptLevel);
  if (OptimizationLevel != llvm::OptimizationLevel::O0)
    Optimize(*LLVMModule, *TargetMachine, OptimizationLevel);

  llvm::SmallString<128> NativeObject;
  llvm::StringRef NativeOutput = OutputPath;
  if (!CWrapperSource.empty()) {
    if (auto ErrorCode = llvm::sys::fs::createTemporaryFile("kelyra-native",
                                                            "o", NativeObject))
      return llvm::createStringError(ErrorCode,
                                     "cannot create temporary object");
    NativeOutput = NativeObject;
  }
  llvm::FileRemover RemoveNative(NativeObject);
  std::error_code ErrorCode;
  llvm::ToolOutputFile Output(NativeOutput, ErrorCode, llvm::sys::fs::OF_None);
  if (ErrorCode)
    return llvm::createStringError(ErrorCode, "cannot open output file");

  llvm::legacy::PassManager CodeGen;
  if (TargetMachine->addPassesToEmitFile(CodeGen, Output.os(), nullptr,
                                         llvm::CodeGenFileType::ObjectFile))
    return llvm::createStringError("target cannot emit an object file");
  CodeGen.run(*LLVMModule);
  Output.os().flush();
  if (Output.os().has_error())
    return llvm::createStringError(Output.os().error(),
                                   "cannot write output file");
  Output.keep();
  if (CWrapperSource.empty())
    return llvm::Error::success();

  llvm::SmallString<128> SourcePath, WrapperObject;
  if (auto Code =
          llvm::sys::fs::createTemporaryFile("kelyra-wrapper", "c", SourcePath))
    return llvm::createStringError(Code, "cannot create C wrapper");
  llvm::FileRemover RemoveSource(SourcePath);
  llvm::raw_fd_ostream Source(SourcePath, ErrorCode);
  if (ErrorCode)
    return llvm::createStringError(ErrorCode, "cannot write C wrapper");
  Source << CWrapperSource;
  Source.close();
  if (auto Code = llvm::sys::fs::createTemporaryFile("kelyra-wrapper", "o",
                                                     WrapperObject))
    return llvm::createStringError(Code, "cannot create wrapper object");
  llvm::FileRemover RemoveWrapper(WrapperObject);
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    return llvm::createStringError(Clang.getError(), "cannot find Clang");
  llvm::SmallVector<llvm::StringRef> Compile{*Clang, "-c", SourcePath};
  for (const auto &Argument : CArguments)
    Compile.push_back(Argument);
  Compile.append({"-o", WrapperObject});
  std::string Message;
  if (llvm::sys::ExecuteAndWait(*Clang, Compile, std::nullopt, {}, 0, 0,
                                &Message) != 0)
    return llvm::createStringError("Clang failed to compile C wrapper: %s",
                                   Message.c_str());
  llvm::SmallString<128> Combined;
  llvm::sys::fs::createUniquePath(OutputPath + ".tmp-%%%%%%%%", Combined,
                                  false);
  llvm::FileRemover RemoveCombined(Combined);
  llvm::SmallVector<llvm::StringRef> Link{*Clang,        "-r", NativeObject,
                                          WrapperObject, "-o", Combined};
  if (llvm::sys::ExecuteAndWait(*Clang, Link, std::nullopt, {}, 0, 0,
                                &Message) != 0)
    return llvm::createStringError("Clang failed to combine objects: %s",
                                   Message.c_str());
  if (auto Code = llvm::sys::fs::rename(Combined, OutputPath))
    return llvm::createStringError(Code, "cannot write object file");
  RemoveCombined.releaseFile();
  return llvm::Error::success();
}

llvm::Error codegen::EmitExecutable(mlir::ModuleOp Module,
                                    llvm::StringRef OutputPath,
                                    unsigned OptLevel,
                                    llvm::ArrayRef<std::string> CSources,
                                    llvm::ArrayRef<std::string> CArguments,
                                    llvm::StringRef CWrapperSource) {
  llvm::SmallString<128> ObjectPath;
  if (auto ErrorCode =
          llvm::sys::fs::createTemporaryFile("kelyra", "o", ObjectPath))
    return llvm::createStringError(ErrorCode, "cannot create temporary object");
  llvm::FileRemover RemoveObject(ObjectPath);
  if (auto Error =
          EmitObject(Module, ObjectPath, OptLevel, CWrapperSource, CArguments))
    return Error;

  auto Linker = llvm::sys::findProgramByName(CSources.empty() ? "cc" : "clang");
  if (!Linker)
    return llvm::createStringError(Linker.getError(), "cannot find C compiler");
  llvm::SmallString<128> TemporaryOutput;
  llvm::sys::fs::createUniquePath(OutputPath + ".tmp-%%%%%%%%", TemporaryOutput,
                                  false);
  llvm::FileRemover RemoveOutput(TemporaryOutput);
  llvm::SmallVector<llvm::StringRef> Arguments{*Linker, ObjectPath};
  for (const auto &Source : CSources)
    Arguments.push_back(Source);
  for (const auto &Argument : CArguments)
    Arguments.push_back(Argument);
  Arguments.append({"-o", TemporaryOutput});
  std::string Message;
  const int Status = llvm::sys::ExecuteAndWait(*Linker, Arguments, std::nullopt,
                                               {}, 0, 0, &Message);
  if (Status != 0)
    return llvm::createStringError("linker failed: %s",
                                   Message.empty() ? "non-zero exit status"
                                                   : Message.c_str());
  if (auto ErrorCode = llvm::sys::fs::rename(TemporaryOutput, OutputPath))
    return llvm::createStringError(ErrorCode, "cannot write executable");
  RemoveOutput.releaseFile();
  return llvm::Error::success();
}
