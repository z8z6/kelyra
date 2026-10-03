#include "CodeGen/IRGen.h"
#include "IR/Kelyra.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"

#include <cassert>

using namespace kelyra;

codegen::IRGen::IRGen(mlir::MLIRContext &Context, const sema::Sema &Analysis, unsigned SafeLevel,
                      bool DebugInfo)
    : Context(Context), Analysis(Analysis), SafeLevel(SafeLevel), DebugInfo(DebugInfo),
      Builder(&Context) {
  Context.loadDialect<ir::KelyraDialect,
                      mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect,
                      mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect>();
}

mlir::OwningOpRef<mlir::ModuleOp> codegen::IRGen::Generate(const lex::Node &Module) {
  return Generate({&Module});
}

mlir::OwningOpRef<mlir::ModuleOp>
codegen::IRGen::Generate(llvm::ArrayRef<const lex::Node *> Modules) {
  using K = lex::NodeKind;
  if (Modules.empty())
    return {};
  auto Result = mlir::ModuleOp::create(GetLocation(Modules.front()->Loc));
  Builder.setInsertionPointToEnd(Result.getBody());
  EmitExternalDeclarations();
  EmitReflectionGlobals(Modules, Result);
  for (const auto *Module : Modules) {
    const bool External = ExternalModules.count(Module) != 0;
    for (const auto &Child : Module->children) {
      if (Child->kind == K::ast_class) {
        if (Analysis.IsMetaDeclaration(*Child))
          continue;
        const bool DeclarationOnly =
            External && (!Child->GenericInstance || !EmitExternalGenericInstances);
        const sema::ClassInfo *Class = nullptr;
        for (const auto &[Name, Candidate] : Analysis.GetClasses())
          if (Candidate.Node == Child.get())
            Class = &Candidate;
        assert(Class);
        EmitClass(*Class, DeclarationOnly, Result);
        continue;
      }
      if (Child->kind != K::ast_function)
        continue;
      if (Analysis.IsMetaDeclaration(*Child) || Analysis.IsShaderDeclaration(*Child))
        continue;
      Builder.setInsertionPointToEnd(Result.getBody());
      EmitFunction(
          *Child, nullptr, External && (!Child->GenericInstance || !EmitExternalGenericInstances));
    }
  }
  return Result;
}
