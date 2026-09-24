#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace kelyra;
using K = lex::TokenKind;

namespace {
bool IsTypeNode(lex::TokenKind Kind) {
  using K = lex::TokenKind;
  return Kind == K::ast_type || Kind == K::ast_pointer_type ||
         Kind == K::ast_array_type || Kind == K::ast_result_types ||
         Kind == K::ast_function_type;
}

bool HasTerminator(mlir::Block *Block) {
  return !Block->empty() &&
         Block->back().hasTrait<mlir::OpTrait::IsTerminator>();
}
} // namespace

void codegen::IRGen::EmitBlock(const lex::Node &Block) {
  const auto ParentDebugScope = DebugScope;
  if (DebugScope)
    DebugScope = mlir::LLVM::DILexicalBlockAttr::get(
        DebugScope, GetDebugFile(Block.Loc), Block.Loc.Line, Block.Loc.Column);
  Scopes.emplace_back();
  Cleanups.emplace_back();
  for (std::size_t Index = 0; Index < Block.children.size(); ++Index) {
    const auto &Statement = Block.children[Index];
    if (HasTerminator(Builder.getInsertionBlock()))
      break;
    const bool PreviousInitializing = InitializingField;
    InitializingField =
        InTransferConstructor && CurrentClass &&
        (Cleanups.size() == 2 || InlineConstructorBlock == &Block) &&
        Index < CurrentClass->UserFieldCount;
    EmitStatement(*Statement);
    if (InTransferConstructor && CurrentClass &&
        Index + 1 == CurrentClass->UserFieldCount &&
        !HasTerminator(Builder.getInsertionBlock()))
      EmitVirtualSlots(*CurrentClass, FindVariable("this")->DirectValue,
                       GetLocation(Statement->Loc));
    InitializingField = PreviousInitializing;
  }
  if (!HasTerminator(Builder.getInsertionBlock()))
    EmitCleanups(Cleanups.size() - 1, GetLocation(Block.Loc));
  Cleanups.pop_back();
  Scopes.pop_back();
  DebugScope = ParentDebugScope;
}

void codegen::IRGen::EmitStatement(const lex::Node &Statement) {
  using K = lex::TokenKind;
  using Handler = void (IRGen::*)(const lex::Node &);
  static const std::unordered_map<K, Handler> Handlers = {
      {K::ast_block, &IRGen::EmitBlockStatement},
      {K::ast_let, &IRGen::EmitLetStatement},
      {K::ast_assign, &IRGen::EmitAssignStatement},
      {K::ast_expr_stmt, &IRGen::EmitExpressionStatement},
      {K::ast_asm, &IRGen::EmitAsmStatement},
      {K::ast_return, &IRGen::EmitReturnStatement},
      {K::ast_break, &IRGen::EmitBreakStatement},
      {K::ast_continue, &IRGen::EmitContinueStatement},
      {K::ast_if, &IRGen::EmitIfStatement},
      {K::ast_when, &IRGen::EmitWhenStatement},
      {K::ast_while, &IRGen::EmitWhileStatement},
  };
  const auto It = Handlers.find(Statement.kind);
  assert(It != Handlers.end() &&
         "semantic analysis accepted a statement without an IR handler");
  (this->*It->second)(Statement);
}

void codegen::IRGen::EmitBlockStatement(const lex::Node &Statement) {
  EmitBlock(Statement);
  return;
}

void codegen::IRGen::EmitLetStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  const auto &Name = *Statement.children[0];
  if (Name.kind == K::ast_binding_list) {
    auto Results = EmitExpression(*Statement.children.back());
    for (std::size_t I = 0; I < Name.children.size(); ++I) {
      const auto &Binding = *Name.children[I];
      const auto &Type = Analysis.GetType(Binding);
      auto Address = CreateAlloca(Type, Loc);
      auto Value = mlir::LLVM::ExtractValueOp::create(Builder, Loc, Results, I);
      mlir::LLVM::StoreOp::create(Builder, Loc, Value, Address);
      Scopes.back().emplace(Binding.text, Variable{Type, Address, {}});
      EmitDebugVariable(Binding.text, Binding.Loc, Type, Address);
    }
    return;
  }
  const auto Type = Analysis.GetType(Statement);
  auto Address = CreateAlloca(Type, Loc);
  const bool HasType =
      Statement.children.size() > 1 && IsTypeNode(Statement.children[1]->kind);
  const lex::Node *Initializer = Statement.children.size() > (HasType ? 2u : 1u)
                                     ? Statement.children.back().get()
                                     : nullptr;
  if (Type.IsClass()) {
    const auto &Class = *Analysis.GetClass(Type);
    if (Analysis.GetConstructorCall(*Initializer)) {
      EmitConstruction(*Initializer, Address);
    } else {
      auto [Source, Temporary] = EmitClassSourceAddress(*Initializer);
      EmitTransfer(Class, Address, Source, Temporary, Loc);
      if (Temporary)
        mlir::func::CallOp::create(Builder, Loc, Class.DestructorSymbol,
                                   mlir::TypeRange{}, mlir::ValueRange{Source});
    }
    Scopes.back().emplace(Name.text, Variable{Type, Address, {}});
    Cleanups.back().push_back({Analysis.GetClass(Type), Address});
    EmitDebugVariable(Name.text, Name.Loc, Type, Address);
    return;
  }
  auto Value =
      Initializer
          ? EmitExpression(*Initializer)
          : mlir::LLVM::ZeroOp::create(Builder, Loc, GetType(Type)).getRes();
  mlir::LLVM::StoreOp::create(Builder, Loc, Value, Address);
  Scopes.back().emplace(Name.text, Variable{Type, Address, {}});
  EmitDebugVariable(Name.text, Name.Loc, Type, Address);
  return;
}

void codegen::IRGen::EmitAssignStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  auto Address = EmitAddress(*Statement.children[0]);
  const auto &TargetType = Analysis.GetType(*Statement.children[0]);
  if (TargetType.IsClass()) {
    if (InitializingField &&
        Analysis.GetConstructorCall(*Statement.children[1])) {
      EmitConstruction(*Statement.children[1], Address);
      return;
    }
    const auto &Class = *Analysis.GetClass(TargetType);
    auto [Source, Temporary] = EmitClassSourceAddress(*Statement.children[1]);
    const auto Transfer = [&] {
      if (InitializingField) {
        EmitTransfer(Class, Address, Source, Temporary, Loc);
      } else {
        auto Staging = CreateAlloca(TargetType, Loc);
        EmitTransfer(Class, Staging, Source, Temporary, Loc);
        mlir::func::CallOp::create(Builder, Loc, Class.DestructorSymbol,
                                   mlir::TypeRange{},
                                   mlir::ValueRange{Address});
        EmitTransfer(Class, Address, Staging, true, Loc);
        mlir::func::CallOp::create(Builder, Loc, Class.DestructorSymbol,
                                   mlir::TypeRange{},
                                   mlir::ValueRange{Staging});
      }
      if (Temporary)
        mlir::func::CallOp::create(Builder, Loc, Class.DestructorSymbol,
                                   mlir::TypeRange{}, mlir::ValueRange{Source});
    };
    if (!InitializingField && !Temporary) {
      auto TargetAddress = mlir::LLVM::PtrToIntOp::create(
          Builder, Loc, Builder.getI64Type(), Address);
      auto SourceAddress = mlir::LLVM::PtrToIntOp::create(
          Builder, Loc, Builder.getI64Type(), Source);
      auto Same = mlir::arith::CmpIOp::create(Builder, Loc,
                                              mlir::arith::CmpIPredicate::eq,
                                              TargetAddress, SourceAddress);
      auto *Region = Builder.getInsertionBlock()->getParent();
      auto *Work = new mlir::Block();
      auto *After = new mlir::Block();
      Region->push_back(Work);
      Region->push_back(After);
      mlir::cf::CondBranchOp::create(Builder, Loc, Same, After, Work);
      Builder.setInsertionPointToStart(Work);
      Transfer();
      mlir::cf::BranchOp::create(Builder, Loc, After);
      Builder.setInsertionPointToStart(After);
    } else {
      Transfer();
    }
    return;
  }
  if (Analysis.GetConstructorCall(*Statement.children[1])) {
    EmitConstruction(*Statement.children[1], Address);
    return;
  }
  auto Value = EmitExpression(*Statement.children[1]);
  mlir::LLVM::StoreOp::create(Builder, Loc, Value, Address);
  return;
}

void codegen::IRGen::EmitExpressionStatement(const lex::Node &Statement) {
  const auto &Expression = *Statement.children.front();
  auto Value = EmitExpression(Expression);
  const auto &Type = Analysis.GetType(Expression);
  if (Type.IsClass()) {
    const auto Loc = GetLocation(Expression.Loc);
    auto Address = CreateAlloca(Type, Loc);
    mlir::LLVM::StoreOp::create(Builder, Loc, Value, Address);
    mlir::func::CallOp::create(Builder, Loc,
                               Analysis.GetClass(Type)->DestructorSymbol,
                               mlir::TypeRange{}, mlir::ValueRange{Address});
  }
  return;
}

void codegen::IRGen::EmitAsmStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  llvm::SmallVector<const lex::Node *> Inputs;
  llvm::SmallVector<const lex::Node *> Outputs;
  std::unordered_map<std::string, const lex::Node *> InputsByName;
  std::unordered_map<std::string, const lex::Node *> OutputsByName;
  std::unordered_set<std::string> Options;
  llvm::SmallVector<std::string> Clobbers;
  for (const auto &Child : Statement.children) {
    if (Child->kind == K::ast_asm_input) {
      Inputs.push_back(Child.get());
      InputsByName.emplace(Child->text, Child.get());
    } else if (Child->kind == K::ast_asm_output) {
      Outputs.push_back(Child.get());
      OutputsByName.emplace(Child->text, Child.get());
    } else if (Child->kind == K::ast_asm_option) {
      Options.emplace(Child->text);
      if (Child->text == "clobber")
        for (const auto &Register : Child->children)
          Clobbers.push_back(Register->text);
    }
  }

  auto ExplicitRegister = [](const lex::Node *Binding) -> std::string_view {
    return Binding && !Binding->children.empty()
               ? std::string_view(Binding->children.front()->text)
               : std::string_view();
  };
  auto RegisterFor = [&](const lex::Node *Binding, const auto &OtherBindings) {
    auto Register = ExplicitRegister(Binding);
    if (!Register.empty())
      return Register;
    const auto Other = OtherBindings.find(Binding->text);
    return Other == OtherBindings.end() ? std::string_view()
                                        : ExplicitRegister(Other->second);
  };
  auto RegisterClass = [&](const lex::Node &Binding) {
    return sema::IsFloat(Analysis.GetType(Binding).Element) ? "f" : "r";
  };

  llvm::SmallVector<std::string> Constraints;
  llvm::SmallVector<mlir::Type> OutputTypes;
  std::unordered_map<std::string, std::size_t> OperandIndices;
  std::unordered_map<std::string, std::size_t> OutputRegisters;
  for (std::size_t I = 0; I < Outputs.size(); ++I) {
    const auto *Output = Outputs[I];
    const auto Register = RegisterFor(Output, InputsByName);
    Constraints.push_back(Register.empty()
                              ? std::string("=&") + RegisterClass(*Output)
                              : "=&{" + std::string(Register) + "}");
    OutputTypes.push_back(GetType(Analysis.GetType(*Output)));
    OperandIndices.emplace(Output->text, I);
    if (!Register.empty())
      OutputRegisters.emplace(std::string(Register), I);
  }

  llvm::SmallVector<mlir::Value> InputValues;
  for (const auto *Input : Inputs) {
    std::optional<std::size_t> TiedOutput;
    if (const auto Output = OperandIndices.find(Input->text);
        Output != OperandIndices.end())
      TiedOutput = Output->second;
    const auto Register = RegisterFor(Input, OutputsByName);
    if (!TiedOutput && !Register.empty())
      if (const auto Output = OutputRegisters.find(std::string(Register));
          Output != OutputRegisters.end())
        TiedOutput = Output->second;
    const auto ConstraintIndex = Constraints.size();
    Constraints.push_back(TiedOutput ? std::to_string(*TiedOutput)
                          : Register.empty()
                              ? RegisterClass(*Input)
                              : "{" + std::string(Register) + "}");
    if (!OperandIndices.contains(Input->text))
      OperandIndices.emplace(Input->text,
                             TiedOutput ? *TiedOutput : ConstraintIndex);
    auto *Variable = FindVariable(Input->text);
    InputValues.push_back(
        Variable->Address ? mlir::LLVM::LoadOp::create(
                                Builder, Loc, GetType(Variable->SemanticType),
                                Variable->Address)
                                .getResult()
                          : Variable->DirectValue);
  }

  auto AddClobber = [&](std::string_view Register) {
    const auto Constraint = "~{" + std::string(Register) + "}";
    if (std::find(Constraints.begin(), Constraints.end(), Constraint) ==
        Constraints.end())
      Constraints.push_back(Constraint);
  };
  for (const auto &Clobber : Clobbers)
    AddClobber(Clobber);
  if (!Options.contains("nomem"))
    AddClobber("memory");
  if (!Options.contains("preserves_flags"))
    AddClobber("cc");

  std::string Asm;
  for (std::size_t I = 0; I < Statement.text.size(); ++I) {
    if (Statement.text[I] == '{' && I + 1 < Statement.text.size() &&
        Statement.text[I + 1] == '{') {
      Asm.push_back('{');
      ++I;
      continue;
    }
    if (Statement.text[I] == '}' && I + 1 < Statement.text.size() &&
        Statement.text[I + 1] == '}') {
      Asm.push_back('}');
      ++I;
      continue;
    }
    if (Statement.text[I] != '{') {
      Asm.push_back(Statement.text[I]);
      continue;
    }
    const auto End = Statement.text.find('}', I + 1);
    const auto Name = Statement.text.substr(I + 1, End - I - 1);
    Asm += '$' + std::to_string(OperandIndices.at(Name));
    I = End;
  }
  const auto First = Asm.find_first_not_of(" \t\r\n");
  const auto Last = Asm.find_last_not_of(" \t\r\n");
  Asm = First == std::string::npos ? std::string()
                                   : Asm.substr(First, Last - First + 1);
  std::string ConstraintText;
  for (const auto &Constraint : Constraints) {
    if (!ConstraintText.empty())
      ConstraintText += ',';
    ConstraintText += Constraint;
  }

  mlir::Type ResultType;
  if (OutputTypes.size() == 1)
    ResultType = OutputTypes.front();
  else if (!OutputTypes.empty())
    ResultType = mlir::LLVM::LLVMStructType::getLiteral(&Context, OutputTypes);
  mlir::LLVM::AsmDialectAttr Dialect;
  if (Options.contains("intel"))
    Dialect = mlir::LLVM::AsmDialectAttr::get(&Context,
                                              mlir::LLVM::AsmDialect::AD_Intel);
  auto InlineAsm = mlir::LLVM::InlineAsmOp::create(
      Builder, Loc, ResultType, InputValues, Asm, ConstraintText,
      /*has_side_effects=*/true, /*is_align_stack=*/false,
      mlir::LLVM::TailCallKind::None, /*convergent=*/false, Dialect,
      mlir::ArrayAttr());
  for (std::size_t I = 0; I < Outputs.size(); ++I) {
    mlir::Value Value = OutputTypes.size() == 1
                            ? InlineAsm.getRes()
                            : mlir::LLVM::ExtractValueOp::create(
                                  Builder, Loc, InlineAsm.getRes(), I)
                                  .getResult();
    mlir::LLVM::StoreOp::create(Builder, Loc, Value,
                                FindVariable(Outputs[I]->text)->Address);
  }
  return;
}

void codegen::IRGen::EmitReturnStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  llvm::SmallVector<mlir::Value> Results;
  if (Statement.children.size() > 1) {
    mlir::Value Values = mlir::LLVM::UndefOp::create(
        Builder, Loc, GetType(Analysis.GetType(Statement)));
    for (std::size_t I = 0; I < Statement.children.size(); ++I)
      Values = mlir::LLVM::InsertValueOp::create(
          Builder, Loc, Values, EmitExpression(*Statement.children[I]), I);
    Results.push_back(Values);
  } else if (!Statement.children.empty()) {
    const auto &Value = *Statement.children.front();
    Results.push_back(Analysis.GetType(Value).IsClass()
                          ? EmitClassArgument(Value)
                          : EmitExpression(Value));
  }
  EmitCleanups(0, Loc);
  if (ActiveDestructor)
    EmitFieldDestructors(*ActiveDestructor, FindVariable("this")->DirectValue,
                         Loc);
  mlir::func::ReturnOp::create(Builder, Loc, Results);
  return;
}

void codegen::IRGen::EmitBreakStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  EmitCleanups(Loops.back().CleanupDepth, Loc);
  mlir::cf::BranchOp::create(Builder, Loc, Loops.back().Break);
  return;
}

void codegen::IRGen::EmitContinueStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  EmitCleanups(Loops.back().CleanupDepth, Loc);
  mlir::cf::BranchOp::create(Builder, Loc, Loops.back().Continue);
  return;
}

void codegen::IRGen::EmitIfStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  auto *Region = Builder.getInsertionBlock()->getParent();
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

void codegen::IRGen::EmitWhenStatement(const lex::Node &Statement) {
  const auto *Branch = Analysis.GetWhenBranch(Statement);
  if (!Branch)
    return;
  if (Branch->kind == K::ast_when)
    EmitStatement(*Branch);
  else
    EmitBlock(*Branch);
}

void codegen::IRGen::EmitWhileStatement(const lex::Node &Statement) {
  const auto Loc = GetLocation(Statement.Loc);
  auto *Region = Builder.getInsertionBlock()->getParent();
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
  Loops.push_back({After, Header, Cleanups.size()});
  EmitBlock(*Statement.children[1]);
  Loops.pop_back();
  if (!HasTerminator(Builder.getInsertionBlock()))
    mlir::cf::BranchOp::create(Builder, Loc, Header);
  Builder.setInsertionPointToStart(After);
}
