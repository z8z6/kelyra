// Backend-neutral shader operations and types in the Kelyra dialect.
//
// Kelyra.td defines the dialect and its shared type base before including this
// file. Keep target API semantics in lowering code, not in these definitions.

def Kelyra_ShaderRecordType : Kelyra_Type<"ShaderRecord", "shader_record"> {
  let summary = "Named shader data record";
  let parameters = (ins "::mlir::StringAttr":$name);
  let assemblyFormat = "`<` $name `>`";
}

def Kelyra_ShaderRefType : Kelyra_Type<"ShaderRef", "shader_ref"> {
  let summary = "Mutable shader local reference";
  let parameters = (ins "::mlir::Type":$valueType);
  let assemblyFormat = "`<` $valueType `>`";
}

def Kelyra_ShaderRecordOp : Op<Kelyra_Dialect, "shader.record", [SymbolName]> {
  let summary = "Named shader data record and interface decorations";
  let arguments = (ins SymbolNameAttr:$sym_name, ArrayAttr:$fields,
                   ArrayAttr:$field_types, ArrayAttr:$decorations);
}

def Kelyra_ShaderFunctionOp : Op<Kelyra_Dialect, "shader.func", [SymbolName]> {
  let summary = "Backend-neutral shader function";
  let arguments = (ins SymbolNameAttr:$sym_name, TypeAttr:$function_type,
                   ArrayAttr:$parameter_names, ArrayAttr:$input_decorations,
                   StrAttr:$stage);
  let regions = (region AnyRegion:$body);
}

def Kelyra_ShaderLiteralOp : Op<Kelyra_Dialect, "shader.literal", [Pure]> {
  let arguments = (ins StrAttr:$text);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderUnaryOp : Op<Kelyra_Dialect, "shader.unary", [Pure]> {
  let arguments = (ins AnyType:$input, StrAttr:$opcode);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderBinaryOp : Op<Kelyra_Dialect, "shader.binary", [Pure]> {
  let arguments = (ins AnyType:$lhs, AnyType:$rhs, StrAttr:$opcode);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderCastOp : Op<Kelyra_Dialect, "shader.cast", [Pure]> {
  let arguments = (ins AnyType:$input);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderConstructOp : Op<Kelyra_Dialect, "shader.construct", [Pure]> {
  let arguments = (ins Variadic<AnyType>:$fields, FlatSymbolRefAttr:$record);
  let results = (outs Kelyra_ShaderRecordType:$result);
}

def Kelyra_ShaderExtractOp : Op<Kelyra_Dialect, "shader.extract", [Pure]> {
  let arguments = (ins AnyType:$record, StrAttr:$field);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderCallOp : Op<Kelyra_Dialect, "shader.call"> {
  let arguments = (ins Variadic<AnyType>:$operands, FlatSymbolRefAttr:$callee);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderIntrinsicOp : Op<Kelyra_Dialect, "shader.intrinsic", [Pure]> {
  let summary = "Backend-neutral shader intrinsic";
  let arguments = (ins Variadic<AnyType>:$operands, StrAttr:$name);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderVarOp : Op<Kelyra_Dialect, "shader.var"> {
  let arguments = (ins Optional<AnyType>:$initial, StrAttr:$name,
                   BoolAttr:$parameter);
  let results = (outs Kelyra_ShaderRefType:$result);
}

def Kelyra_ShaderLoadOp : Op<Kelyra_Dialect, "shader.load"> {
  let arguments = (ins Kelyra_ShaderRefType:$reference);
  let results = (outs AnyType:$result);
}

def Kelyra_ShaderStoreOp : Op<Kelyra_Dialect, "shader.store"> {
  let arguments = (ins Kelyra_ShaderRefType:$reference, AnyType:$value);
}

def Kelyra_ShaderStoreFieldOp : Op<Kelyra_Dialect, "shader.store_field"> {
  let arguments = (ins Kelyra_ShaderRefType:$reference, AnyType:$value,
                   ArrayAttr:$path);
}

def Kelyra_ShaderEvalOp : Op<Kelyra_Dialect, "shader.eval"> {
  let arguments = (ins AnyType:$value);
}

def Kelyra_ShaderIfOp : Op<Kelyra_Dialect, "shader.if"> {
  let arguments = (ins I1:$condition);
  let regions = (region AnyRegion:$thenRegion, AnyRegion:$elseRegion);
}

def Kelyra_ShaderWhileOp : Op<Kelyra_Dialect, "shader.while"> {
  let regions = (region AnyRegion:$condition, AnyRegion:$body);
}

def Kelyra_ShaderYieldOp : Op<Kelyra_Dialect, "shader.yield", [Terminator]> {
  let arguments = (ins Variadic<AnyType>:$values);
}

def Kelyra_ShaderReturnOp : Op<Kelyra_Dialect, "shader.return", [Terminator]> {
  let arguments = (ins AnyType:$value);
}

def Kelyra_ShaderBreakOp : Op<Kelyra_Dialect, "shader.break", [Terminator]> {}
def Kelyra_ShaderContinueOp : Op<Kelyra_Dialect, "shader.continue", [Terminator]> {}
