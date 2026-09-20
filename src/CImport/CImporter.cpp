#include "CImport/CImporter.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MemoryBuffer.h"

#include <optional>

using namespace kelyra;

namespace {
std::optional<sema::Type> ConvertType(clang::ASTContext &Context,
                                      clang::QualType Type) {
  using C = clang::BuiltinType;
  using K = sema::BuiltinType;
  if (Type->isVoidType())
    return sema::Type{K::Void, {}};
  const auto Spelling = Type.getAsString();
  if (Type->isPointerType()) {
    const auto Pointee = Type->getPointeeType();
    std::optional<sema::Type> Result;
    if (Pointee->isVoidType()) {
      Result = sema::Type{K::CRecord, {}};
      Result->CName = "void";
    } else {
      Result = ConvertType(Context, Pointee);
    }
    if (!Result || Pointee->isFunctionType())
      return std::nullopt;
    ++Result->PointerDepth;
    Result->BitWidth = Context.getTypeSize(Type);
    Result->Alignment = Context.getTypeAlign(Type) / 8;
    Result->CSpelling = Spelling;
    return Result;
  }
  if (const auto *Record = Type->getAsRecordDecl()) {
    sema::Type Result{K::CRecord, {}};
    Result.CName = Record->getNameAsString();
    Result.CSpelling = Spelling;
    if (!Type->isIncompleteType()) {
      Result.BitWidth = Context.getTypeSize(Type);
      Result.Alignment = Context.getTypeAlign(Type) / 8;
    }
    return Result;
  }
  K Result;
  if (Spelling == "size_t")
    Result = K::CSize;
  else if (Spelling == "ptrdiff_t")
    Result = K::CPtrdiff;
  else if (Spelling == "wchar_t")
    Result = K::CWChar;
  else {
    const auto *Builtin = Type.getCanonicalType()->getAs<C>();
    if (!Builtin)
      return std::nullopt;
    switch (Builtin->getKind()) {
    case C::Char_S:
    case C::Char_U:
      Result = K::CChar;
      break;
    case C::SChar:
      Result = K::CSChar;
      break;
    case C::UChar:
      Result = K::CUChar;
      break;
    case C::Short:
      Result = K::CShort;
      break;
    case C::Int:
      Result = K::CInt;
      break;
    case C::UInt:
      Result = K::CUInt;
      break;
    case C::Long:
      Result = K::CLong;
      break;
    case C::LongLong:
      Result = K::CLongLong;
      break;
    case C::Bool:
      Result = K::CBool;
      break;
    default:
      return std::nullopt;
    }
  }
  sema::Type Converted{Result, {}};
  Converted.BitWidth = Context.getTypeSize(Type);
  Converted.Alignment = Context.getTypeAlign(Type) / 8;
  Converted.CSpelling = Spelling;
  return Converted;
}
} // namespace

cimport::ImportResult
cimport::ImportHeaders(const std::vector<std::string> &Headers,
                       const std::vector<std::string> &Arguments) {
  ImportResult Result;
  llvm::StringSet<> Seen;
  llvm::StringSet<> SeenTypes;
  for (const auto &Header : Headers) {
    auto Buffer = llvm::MemoryBuffer::getFile(Header);
    if (!Buffer) {
      Result.Diagnostics.push_back("cannot read C header: " + Header);
      continue;
    }
    std::vector<std::string> Args{"-x", "c", "-std=c17"};
    Args.insert(Args.end(), Arguments.begin(), Arguments.end());
    auto Ast = clang::tooling::buildASTFromCodeWithArgs((*Buffer)->getBuffer(),
                                                        Args, Header, "clang");
    if (!Ast) {
      Result.Diagnostics.push_back("Clang could not parse C header: " + Header);
      continue;
    }

    auto &Context = Ast->getASTContext();
    const auto &Sources = Context.getSourceManager();
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Typedef = llvm::dyn_cast<clang::TypedefNameDecl>(Declaration);
      if (!Typedef || !Sources.isWrittenInMainFile(Typedef->getLocation()))
        continue;
      auto Type = ConvertType(Context, Typedef->getUnderlyingType());
      if (!Type ||
          (Type->Element != sema::BuiltinType::CRecord && !Type->IsPointer()))
        continue;
      if (Type->CName.empty())
        Type->CName = Typedef->getNameAsString();
      Type->CSpelling = Typedef->getNameAsString();
      const auto Name = "c." + Typedef->getNameAsString();
      if (SeenTypes.insert(Name).second)
        Result.Types.push_back(sema::ExternalType{Name, std::move(*Type)});
    }
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Record = llvm::dyn_cast<clang::RecordDecl>(Declaration);
      if (!Record || Record->getName().empty() ||
          !Record->isCompleteDefinition() ||
          !Sources.isWrittenInMainFile(Record->getLocation()))
        continue;
      auto Type = ConvertType(Context, Context.getCanonicalTagType(Record));
      const auto Name = "c." + Record->getNameAsString();
      if (Type && SeenTypes.insert(Name).second)
        Result.Types.push_back(sema::ExternalType{Name, std::move(*Type)});
    }
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Function = llvm::dyn_cast<clang::FunctionDecl>(Declaration);
      if (!Function || Function->getIdentifier() == nullptr ||
          !Sources.isWrittenInMainFile(Function->getLocation()) ||
          !Function->hasExternalFormalLinkage())
        continue;
      const auto Name = Function->getNameAsString();
      if (!Seen.insert(Name).second)
        continue;
      auto Return = ConvertType(Context, Function->getReturnType());
      if (!Return) {
        Result.Diagnostics.push_back("unsupported C return type for " + Name);
        continue;
      }
      sema::ExternalFunction Imported;
      Imported.Name = Name;
      Imported.Header = Header;
      Imported.Return = *Return;
      Imported.Variadic = Function->isVariadic();
      Imported.CallingConvention =
          Function->getType()->castAs<clang::FunctionType>()->getCallConv();
      if (Imported.CallingConvention != clang::CC_C) {
        Result.Diagnostics.push_back("unsupported C calling convention for " +
                                     Name);
        continue;
      }
      bool Supported = true;
      for (const auto *Parameter : Function->parameters()) {
        auto Type = ConvertType(Context, Parameter->getType());
        if (!Type) {
          Result.Diagnostics.push_back("unsupported C parameter type for " +
                                       Name);
          Supported = false;
          break;
        }
        Imported.Parameters.push_back(*Type);
      }
      if (Supported)
        Result.Functions.push_back(std::move(Imported));
    }
  }
  return Result;
}
