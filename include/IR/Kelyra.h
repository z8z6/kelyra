#pragma once

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "KelyraDialect.h.inc"

#define GET_TYPEDEF_CLASSES
#include "KelyraTypes.h.inc"

#define GET_OP_CLASSES
#include "KelyraOps.h.inc"
