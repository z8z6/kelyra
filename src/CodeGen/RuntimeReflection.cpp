#include "CodeGen/IRGen.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include <cstdint>
#include <string>

using namespace kelyra;

namespace {
std::string TypeName(const sema::Type &Type) {
  if (Type.IsPointer())
    return "*" + TypeName(Type.Pointee());
  if (Type.IsArray())
    return "[" + std::to_string(Type.ArrayLength()) + "]" +
           TypeName(Type.Indexed());
  if (Type.IsClass())
    return Type.ClassName;
  if (!Type.CName.empty())
    return Type.CName;
  return std::string(sema::GetBuiltinTypeInfo(Type.Element).Name);
}

std::string Encode(std::string_view Name) {
  constexpr char Hex[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Name.size() * 2);
  for (unsigned char Byte : Name) {
    Result.push_back(Hex[Byte >> 4]);
    Result.push_back(Hex[Byte & 15]);
  }
  return Result;
}

std::uint64_t TypeId(std::string_view Name) {
  std::uint64_t Hash = 14695981039346656037ULL;
  for (unsigned char Byte : Name) {
    Hash ^= Byte;
    Hash *= 1099511628211ULL;
  }
  return Hash;
}

void AppendU32(std::string &Bytes, std::uint32_t Value) {
  for (unsigned I = 0; I < 4; ++I)
    Bytes.push_back(static_cast<char>(Value >> (I * 8)));
}

void AppendU64(std::string &Bytes, std::uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I)
    Bytes.push_back(static_cast<char>(Value >> (I * 8)));
}

std::string MakeBlob(const sema::Type &Type, const sema::ClassInfo *Class,
                     const sema::ReflectionDatabase &Reflection) {
  const auto Name = TypeName(Type);
  std::uint32_t Count = 0;
  if (Class)
    for (const auto &Field : Class->Fields)
      if (const auto Id = Reflection.GetId(*Field.Node);
          Id && Reflection.Get(*Id).RuntimeReflected)
        ++Count;
  std::string Bytes;
  AppendU32(Bytes, 0x46524c4b); // "KLRF" in little-endian order.
  AppendU32(Bytes, 1);          // Descriptor ABI version.
  AppendU64(Bytes, TypeId(Name));
  AppendU32(Bytes, Name.size());
  AppendU32(Bytes, Count);
  Bytes.append(Name);
  Bytes.push_back('\0');
  if (Class)
    for (const auto &Field : Class->Fields) {
      const auto Id = Reflection.GetId(*Field.Node);
      if (!Id || !Reflection.Get(*Id).RuntimeReflected)
        continue;
      const auto FieldTypeName = TypeName(Field.Value);
      AppendU64(Bytes, TypeId(FieldTypeName));
      AppendU64(Bytes, Field.Offset);
      AppendU32(Bytes, Field.Name.size());
      AppendU32(Bytes, FieldTypeName.size());
      Bytes.append(Field.Name);
      Bytes.push_back('\0');
      Bytes.append(FieldTypeName);
      Bytes.push_back('\0');
    }
  return Bytes;
}

mlir::LLVM::GlobalOp
CreateBlobGlobal(mlir::OpBuilder &Builder, mlir::Location Loc,
                 std::string_view Symbol, std::string_view Bytes, bool External,
                 bool DeclarationOnly, bool Generic = false) {
  auto Array = mlir::LLVM::LLVMArrayType::get(
      Builder.getI8Type(), DeclarationOnly ? 1 : Bytes.size());
  return mlir::LLVM::GlobalOp::create(
      Builder, Loc, Array, true,
      Generic    ? mlir::LLVM::Linkage::LinkonceODR
      : External ? mlir::LLVM::Linkage::External
                 : mlir::LLVM::Linkage::Private,
      Symbol,
      DeclarationOnly ? mlir::Attribute() : Builder.getStringAttr(Bytes), 1);
}
} // namespace

void codegen::IRGen::EmitReflectionGlobals(
    llvm::ArrayRef<const lex::Node *> Modules, mlir::ModuleOp Output) {
  mlir::OpBuilder GlobalBuilder(&Context);
  GlobalBuilder.setInsertionPointToStart(Output.getBody());
  for (const auto *Module : Modules) {
    const bool External = ExternalModules.count(Module) != 0;
    for (const auto &Child : Module->children) {
      if (Child->kind != lex::TokenKind::ast_class)
        continue;
      const auto *Class = [&]() -> const sema::ClassInfo * {
        for (const auto &[Name, Candidate] : Analysis.GetClasses())
          if (Candidate.Node == Child.get())
            return &Candidate;
        return nullptr;
      }();
      if (!Class)
        continue;
      const auto Id = Analysis.GetReflection().GetId(*Child);
      if (!Id || !Analysis.GetReflection().Get(*Id).RuntimeReflected)
        continue;
      sema::Type Type{sema::BuiltinType::Class, {}};
      Type.ClassName = Class->QualifiedName;
      const auto Bytes = MakeBlob(Type, Class, Analysis.GetReflection());
      CreateBlobGlobal(GlobalBuilder, GetLocation(Child->Loc),
                       "__kelyra_reflect_v1_" + Encode(Class->QualifiedName),
                       Bytes, true, External && !Child->GenericInstance,
                       Child->GenericInstance);
    }
  }
}

bool codegen::IRGen::EmitReflectIntrinsic(const lex::Node &Function,
                                          mlir::func::FuncOp Func,
                                          bool DeclarationOnly) {
  const auto Id = Analysis.GetReflection().GetId(Function);
  if (!Id || !Analysis.GetReflection().Get(*Id).QualifiedName.starts_with(
                 "std.reflect.__type_data__G"))
    return false;
  if (DeclarationOnly) {
    Func.setPrivate();
    return true;
  }
  Func.setPrivate();
  const lex::Node *Parameter = nullptr;
  for (const auto &Child : Function.children)
    if (Child->kind == lex::TokenKind::ast_parameter) {
      Parameter = Child.get();
      break;
    }
  assert(Parameter && "reflection intrinsic needs a typed pointer");
  const auto Type = Analysis.GetType(*Parameter).Pointee();
  const auto Name = TypeName(Type);
  const auto Symbol = "__kelyra_reflect_v1_" + Encode(Name);
  const auto LocalSymbol = "__kelyra_reflect_v1_local_" + Encode(Name);
  auto Module = Func->getParentOfType<mlir::ModuleOp>();
  auto Global = Module.lookupSymbol<mlir::LLVM::GlobalOp>(Symbol);
  if (!Global)
    Global = Module.lookupSymbol<mlir::LLVM::GlobalOp>(LocalSymbol);
  if (!Global) {
    const auto Bytes = MakeBlob(Type, nullptr, Analysis.GetReflection());
    mlir::OpBuilder GlobalBuilder(&Context);
    GlobalBuilder.setInsertionPointToStart(Module.getBody());
    Global = CreateBlobGlobal(GlobalBuilder, GetLocation(Function.Loc),
                              LocalSymbol, Bytes, false, false);
  }
  auto *Entry = Func.addEntryBlock();
  Builder.setInsertionPointToStart(Entry);
  auto Pointer = mlir::LLVM::AddressOfOp::create(
      Builder, GetLocation(Function.Loc),
      mlir::LLVM::LLVMPointerType::get(&Context), Global.getSymNameAttr());
  mlir::func::ReturnOp::create(Builder, GetLocation(Function.Loc),
                               Pointer.getResult());
  return true;
}
