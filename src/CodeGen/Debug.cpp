#include "CodeGen/IRGen.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace kelyra;
namespace di = mlir::LLVM;

di::DIFileAttr codegen::IRGen::GetDebugFile(const lex::Location &Loc) {
  llvm::SmallString<256> Path(Loc.File.empty() ? "<unknown>" : Loc.File);
  llvm::sys::fs::make_absolute(Path);
  return di::DIFileAttr::get(&Context, llvm::sys::path::filename(Path),
                             llvm::sys::path::parent_path(Path));
}

di::DITypeAttr codegen::IRGen::GetDebugType(const sema::Type &Type) {
  using namespace llvm::dwarf;
  if (Type.IsVoid() || Type.IsResults())
    return di::DINullTypeAttr::get(&Context);
  if (Type.IsArray()) {
    auto Element = Type;
    Element.Dimensions.clear();
    llvm::SmallVector<di::DINodeAttr> Bounds;
    for (auto Length : Type.Dimensions)
      Bounds.push_back(
          di::DISubrangeAttr::get(&Context, Builder.getI64IntegerAttr(Length),
                                  Builder.getI64IntegerAttr(0), {}, {}));
    return di::DICompositeTypeAttr::get(
        &Context, DW_TAG_array_type, {}, {}, 0, {}, GetDebugType(Element),
        di::DIFlags::Zero, 0, 0, {}, {}, {}, {}, {}, {}, Bounds);
  }
  if (Type.IsPointer()) {
    auto Pointee = Type;
    --Pointee.PointerDepth;
    if (!Pointee.IsPointer()) {
      Pointee.BitWidth = 0;
      Pointee.Alignment = 0;
    }
    return di::DIDerivedTypeAttr::get(&Context, DW_TAG_pointer_type, {}, {}, 0,
                                      {}, GetDebugType(Pointee),
                                      sizeof(void *) * 8, alignof(void *) * 8,
                                      0, std::nullopt, di::DIFlags::Zero, {});
  }
  if (Type.IsFunction()) {
    llvm::SmallVector<di::DITypeAttr> Types{GetDebugType(Type.Results.front())};
    for (const auto &Parameter : Type.Parameters)
      Types.push_back(GetDebugType(Parameter));
    auto Signature =
        di::DISubroutineTypeAttr::get(&Context, DW_CC_normal, Types);
    return di::DIDerivedTypeAttr::get(&Context, DW_TAG_pointer_type, {}, {}, 0,
                                      {}, Signature, sizeof(void *) * 8,
                                      alignof(void *) * 8, 0, std::nullopt,
                                      di::DIFlags::Zero, {});
  }
  if (Type.IsClass()) {
    if (const auto Found = DebugClasses.find(Type.ClassName);
        Found != DebugClasses.end())
      return Found->second;
    const auto &Class = *Analysis.GetClass(Type);
    auto RecId = mlir::DistinctAttr::create(Builder.getUnitAttr());
    auto Self =
        mlir::cast<di::DITypeAttr>(di::DICompositeTypeAttr::getRecSelf(RecId));
    DebugClasses.emplace(Type.ClassName, Self);
    llvm::SmallVector<di::DINodeAttr> Fields;
    for (const auto &Field : Class.Fields) {
      auto FieldType = GetDebugType(Field.Value);
      Fields.push_back(di::DIDerivedTypeAttr::get(
          &Context, DW_TAG_member, Builder.getStringAttr(Field.Name),
          GetDebugFile(Field.Node->Loc), Field.Node->Loc.Line, {}, FieldType, 0,
          0, Field.Offset * 8, std::nullopt, di::DIFlags::Zero, {}));
    }
    auto Result = di::DICompositeTypeAttr::get(
        &Context, RecId, false, DW_TAG_structure_type,
        Builder.getStringAttr(Class.QualifiedName),
        GetDebugFile(Class.Node->Loc), Class.Node->Loc.Line, {}, {},
        di::DIFlags::Zero, Class.Size * 8, Class.Alignment * 8, {}, {}, {}, {},
        {}, {}, Fields);
    DebugClasses[Type.ClassName] = Result;
    return Result;
  }
  if (Type.IsRecord())
    return di::DICompositeTypeAttr::get(
        &Context, DW_TAG_structure_type, Builder.getStringAttr(Type.CName), {},
        0, {}, {}, di::DIFlags::FwdDecl, sema::GetBitWidth(Type),
        Type.Alignment * 8, {}, {}, {}, {}, {}, {}, {});
  const auto &Info = sema::GetBuiltinTypeInfo(Type.Element);
  const auto Encoding = sema::IsFloat(Type.Element)           ? DW_ATE_float
                        : Info.Class == sema::TypeClass::Bool ? DW_ATE_boolean
                        : sema::IsSignedInteger(Type.Element) ? DW_ATE_signed
                                                              : DW_ATE_unsigned;
  return di::DIBasicTypeAttr::get(
      &Context, DW_TAG_base_type, Builder.getStringAttr(Info.Name),
      std::max(8u, sema::GetBitWidth(Type)), Encoding);
}

void codegen::IRGen::BeginDebugFunction(mlir::Operation *Function,
                                        const lex::Node &Node) {
  if (!DebugInfo)
    return;
  auto File = GetDebugFile(Node.Loc);
  auto Unit = di::DICompileUnitAttr::get(
      mlir::DistinctAttr::create(Builder.getUnitAttr()), llvm::dwarf::DW_LANG_C,
      File, Builder.getStringAttr("Kelyra"), false, di::DIEmissionKind::Full);
  llvm::SmallVector<di::DITypeAttr> Types{GetDebugType(Analysis.GetType(Node))};
  if (CurrentClass) {
    sema::Type Receiver{sema::BuiltinType::Class, {}};
    Receiver.ClassName = CurrentClass->QualifiedName;
    Receiver.PointerDepth = 1;
    Types.push_back(GetDebugType(Receiver));
  }
  for (const auto &Child : Node.children)
    if (Child->kind == lex::TokenKind::ast_parameter)
      Types.push_back(GetDebugType(Analysis.GetType(*Child)));
  auto Signature =
      di::DISubroutineTypeAttr::get(&Context, llvm::dwarf::DW_CC_normal, Types);
  auto Scope = di::DISubprogramAttr::get(
      &Context, mlir::DistinctAttr::create(Builder.getUnitAttr()), Unit, File,
      Builder.getStringAttr(Node.text),
      Builder.getStringAttr(Analysis.GetSymbol(Node)), File, Node.Loc.Line,
      Node.Loc.Line, di::DISubprogramFlags::Definition, Signature, {}, {});
  Function->setLoc(mlir::FusedLoc::get(&Context, {Function->getLoc()}, Scope));
  DebugScope = Scope;
}

void codegen::IRGen::EmitDebugVariable(std::string_view Name,
                                       const lex::Location &Loc,
                                       const sema::Type &Type,
                                       mlir::Value Storage, unsigned Argument,
                                       bool DirectValue) {
  if (!DebugScope || !di::isCompatibleType(Storage.getType()))
    return;
  auto Variable = di::DILocalVariableAttr::get(
      &Context, DebugScope, Builder.getStringAttr(Name), GetDebugFile(Loc),
      Loc.Line, Argument, 0, GetDebugType(Type), di::DIFlags::Zero);
  auto Expression = di::DIExpressionAttr::get(&Context, {});
  if (DirectValue)
    di::DbgValueOp::create(Builder, GetLocation(Loc), Storage, Variable,
                           Expression);
  else
    di::DbgDeclareOp::create(Builder, GetLocation(Loc), Storage, Variable,
                             Expression);
}
