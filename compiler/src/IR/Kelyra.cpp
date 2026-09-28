#include "IR/Kelyra.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

#include "KelyraDialect.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "KelyraTypes.cpp.inc"

void kelyra::ir::KelyraDialect::initialize() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "KelyraTypes.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "KelyraOps.cpp.inc"
      >();
}

#define GET_OP_CLASSES
#include "KelyraOps.cpp.inc"
