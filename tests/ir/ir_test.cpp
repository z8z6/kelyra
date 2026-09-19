#include "IR/Kelyra.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>
#include <string>

TEST(KelyraIR, AddRoundTrip) {
  mlir::MLIRContext context;
  context.loadDialect<kelyra::ir::KelyraDialect, mlir::func::FuncDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(R"(
    module {
      func.func @add(%a: i32, %b: i32) -> i32 {
        %sum = kelyra.add %a, %b : i32
        return %sum : i32
      }
    }
  )", &context);
  ASSERT_TRUE(bool(module) && mlir::succeeded(mlir::verify(*module))) << "valid add parses and verifies";
  unsigned adds = 0;
  module->walk([&](kelyra::ir::AddOp add) {
    ++adds;
    ASSERT_TRUE(add.getLhs().getType().isInteger(32) && add.getRhs().getType() == add.getResult().getType()) << "typed add accessors";
  });
  ASSERT_TRUE(adds == 1) << "add registered as a typed operation";
  std::string printed;
  llvm::raw_string_ostream os(printed);
  module->print(os);
  ASSERT_TRUE(printed.find("kelyra.add") != std::string::npos) << "custom assembly printer";
  ASSERT_TRUE(bool(mlir::parseSourceString<mlir::ModuleOp>(printed, &context))) << "printed IR round trips";
}

TEST(KelyraIR, RejectInvalidAdd) {
  mlir::MLIRContext context;
  context.loadDialect<kelyra::ir::KelyraDialect, mlir::func::FuncDialect>();
  unsigned diagnostics = 0;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &) {
    ++diagnostics;
    return mlir::success();
  });
  // Generic syntax bypasses the custom parser so the operation verifier is tested.
  for (const char *invalid : {
      R"(module { func.func @bad(%a: f32, %b: f32) { %r = "kelyra.add"(%a, %b) : (f32, f32) -> f32 return } })",
      R"(module { func.func @bad(%a: i32, %b: i64) { %r = "kelyra.add"(%a, %b) : (i32, i64) -> i32 return } })",
      R"(module { func.func @bad(%a: i32, %b: i32) { %r = "kelyra.add"(%a, %b) : (i32, i32) -> i64 return } })",
      R"(module { func.func @bad(%a: i32) { %r = "kelyra.add"(%a) : (i32) -> i32 return } })"}) {
    SCOPED_TRACE(invalid);
    const auto before = diagnostics;
    ASSERT_FALSE(mlir::parseSourceString<mlir::ModuleOp>(invalid, &context)) << "invalid add rejected";
    ASSERT_TRUE(diagnostics > before) << "invalid add emits diagnostic";
  }
}
