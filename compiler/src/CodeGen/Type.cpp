#include "CodeGen/IRGen.h"
#include "IR/Kelyra.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>

using namespace kelyra;

mlir::Type codegen::IRGen::GetType(const sema::Type &Type) {
  using T = sema::BuiltinType;
  if (Type.IsResults()) {
    llvm::SmallVector<mlir::Type> Results;
    for (const auto &Result : Type.Results)
      Results.push_back(GetType(Result));
    return mlir::LLVM::LLVMStructType::getLiteral(&Context, Results);
  }
  if (Type.IsArray()) {
    return mlir::LLVM::LLVMArrayType::get(GetType(Type.Indexed()), Type.ArrayLength());
  }
  if (Type.IsSlice()) {
    return mlir::LLVM::LLVMStructType::getLiteral(
        &Context,
        {mlir::LLVM::LLVMPointerType::get(&Context),
         Builder.getIntegerType(Analysis.GetTargetLayout().GetPointerBitWidth())});
  }
  if (Type.IsPointer() && Type.PointerDepth == 1 && Type.Element == T::Class) {
    const auto *Class = Analysis.GetClass(Type.ClassName);
    if (Class && Class->IsInterface) {
      auto Pointer = mlir::LLVM::LLVMPointerType::get(&Context);
      llvm::SmallVector<mlir::Type> Fields(Class->InterfaceMethods.size() + 1, Pointer);
      return mlir::LLVM::LLVMStructType::getLiteral(&Context, Fields);
    }
  }
  if (Type.IsPointer() || Type.IsFunction())
    return mlir::LLVM::LLVMPointerType::get(&Context);
  if (Type.IsClass()) {
    llvm::SmallVector<mlir::Type> Fields;
    const auto &Class = *Analysis.GetClass(Type);
    std::uint64_t Offset = 0;
    for (const auto &Field : Class.Fields) {
      if (Offset < Field.Offset)
        Fields.push_back(
            mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(), Field.Offset - Offset));
      Fields.push_back(GetType(Field.Value));
      auto Element = Field.Value;
      std::uint64_t Count = 1;
      while (Element.IsArray()) {
        Count *= Element.ArrayLength();
        Element = Element.Indexed();
      }
      std::uint64_t ElementSize = Element.IsClass() ? Analysis.GetClass(Element)->Size
                                                    : (Analysis.GetBitWidth(Element) + 7) / 8;
      if (Element.IsPointer() && Element.PointerDepth == 1 && Element.Element == T::Class) {
        const auto *Pointee = Analysis.GetClass(Element.ClassName);
        if (Pointee && Pointee->IsInterface)
          ElementSize = (Pointee->InterfaceMethods.size() + 1) *
                        (Analysis.GetTargetLayout().GetPointerBitWidth() / 8);
      }
      const std::uint64_t Size = Count * ElementSize;
      Offset = Field.Offset + Size;
    }
    if (Offset < Class.Size)
      Fields.push_back(mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(), Class.Size - Offset));
    return mlir::LLVM::LLVMStructType::getLiteral(&Context, Fields, true);
  }
  if (Type.IsRecord())
    return mlir::LLVM::LLVMArrayType::get(Builder.getI8Type(),
                                          (Analysis.GetBitWidth(Type) + 7) / 8);
  if (Type.IsEnum())
    return Builder.getIntegerType(Analysis.GetBitWidth(Type));
  mlir::Type Result;
  if (sema::IsInteger(Type.Element))
    Result = Builder.getIntegerType(Analysis.GetBitWidth(Type));
  else {
    switch (Type.Element) {
    case T::F32:
    case T::CFloat:
      Result = Builder.getF32Type();
      break;
    case T::F64:
    case T::CDouble:
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
      Result = Builder.getIntegerType(Analysis.GetBitWidth(Type));
      break;
    case T::Char:
      Result = Builder.getI32Type();
      break;
    default:
      llvm_unreachable("unhandled builtin type");
    }
  }
  return Result;
}
