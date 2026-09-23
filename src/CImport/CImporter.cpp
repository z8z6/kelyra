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
#include <sstream>

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
    Result->AddPointer();
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
    case C::Float:
      Result = K::CFloat;
      break;
    case C::Double:
      Result = K::CDouble;
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

namespace {
std::optional<std::string> KelyraType(const sema::Type &Type) {
  if (Type.IsPointer()) {
    auto Pointee = KelyraType(Type.Pointee());
    if (!Pointee || *Pointee == "void")
      return std::nullopt;
    return "*" + *Pointee;
  }
  if (Type.IsRecord()) {
    if (Type.CName.empty() || Type.CName == "void")
      return std::nullopt;
    return "c." + Type.CName;
  }
  using T = sema::BuiltinType;
  switch (Type.Element) {
  case T::Void:
    return "void";
  case T::CChar:
    return "c.char";
  case T::CSChar:
    return "c.schar";
  case T::CUChar:
    return "c.uchar";
  case T::CShort:
    return "c.short";
  case T::CInt:
    return "c.int";
  case T::CUInt:
    return "c.uint";
  case T::CLong:
    return "c.long";
  case T::CLongLong:
    return "c.longlong";
  case T::CSize:
    return "c.size";
  case T::CPtrdiff:
    return "c.ptrdiff";
  case T::CBool:
    return "c.bool";
  case T::CFloat:
    return "c.float";
  case T::CDouble:
    return "c.double";
  case T::CWChar:
    return "c.wchar";
  default:
    return std::nullopt;
  }
}

std::string EscapeString(std::string_view Value) {
  std::string Result;
  for (const char Character : Value) {
    if (Character == '\\' || Character == '"')
      Result += '\\';
    Result += Character;
  }
  return Result;
}
} // namespace

std::string
cimport::GenerateDefinitions(const ImportResult &Declarations,
                             const std::vector<std::string> &Headers,
                             const std::string &ModuleName) {
  std::ostringstream Source;
  Source << "module " << ModuleName << ";\n\n";
  for (const auto &Header : Headers)
    Source << "import c \"" << EscapeString(Header) << "\";\n";
  Source << '\n';
  for (const auto &Function : Declarations.Functions) {
    if (Function.Variadic)
      continue;
    auto Return = KelyraType(Function.Return);
    if (!Return)
      continue;
    std::vector<std::string> Parameters;
    for (const auto &Parameter : Function.Parameters) {
      auto Name = KelyraType(Parameter);
      if (!Name)
        break;
      Parameters.push_back(std::move(*Name));
    }
    if (Parameters.size() != Function.Parameters.size())
      continue;
    Source << "pub fn " << Function.Name << '(';
    for (std::size_t I = 0; I < Parameters.size(); ++I) {
      if (I)
        Source << ", ";
      Source << "arg" << I << ": " << Parameters[I];
    }
    Source << ") -> " << *Return << " {\n  ";
    if (*Return != "void")
      Source << "return ";
    Source << "c." << Function.Name << '(';
    for (std::size_t I = 0; I < Parameters.size(); ++I) {
      if (I)
        Source << ", ";
      Source << "arg" << I;
    }
    Source << ");\n}\n\n";
  }
  return Source.str();
}
