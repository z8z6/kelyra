#ifndef KELYRA_IR
#define KELYRA_IR

include "mlir/IR/OpBase.td"
include "mlir/IR/AttrTypeBase.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def Kelyra_Dialect : Dialect {
  let name = "kelyra";
  let cppNamespace = "::kelyra::ir";
  let summary = "Kelyra language operations";
  let useDefaultTypePrinterParser = 1;
}

class Kelyra_Type<string name, string typeMnemonic>
    : TypeDef<Kelyra_Dialect, name> {
  let mnemonic = typeMnemonic;
}

def Kelyra_F256Type : Kelyra_Type<"F256", "f256"> {
  let summary = "256-bit Kelyra floating-point type";
}

def Kelyra_F512Type : Kelyra_Type<"F512", "f512"> {
  let summary = "512-bit Kelyra floating-point type";
}

def Kelyra_AddOp : Op<Kelyra_Dialect, "add",
    [Pure, Commutative, SameOperandsAndResultType]> {
  let summary = "Scalar integer addition with wrapping overflow";
  let description = [{
    Adds two signless integers of the same width. The result has the same
    type and is reduced modulo 2^width. This example operation does not
    establish the overflow policy of Kelyra source-language arithmetic.
  }];
  let arguments = (ins AnySignlessInteger:$lhs, AnySignlessInteger:$rhs);
  let results = (outs AnySignlessInteger:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

#endif
