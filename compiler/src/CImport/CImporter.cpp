#include "CImport/CImporter.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>

using namespace kelyra;

namespace {
std::optional<std::string> FindClangResourceDir() {
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    return std::nullopt;
  llvm::SmallString<128> Output;
  if (llvm::sys::fs::createTemporaryFile("kelyra-clang-resource", "txt",
                                         Output))
    return std::nullopt;
  llvm::FileRemover RemoveOutput(Output);
  const llvm::StringRef Arguments[] = {*Clang, "-print-resource-dir"};
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, Output.str(), std::nullopt};
  if (llvm::sys::ExecuteAndWait(*Clang, Arguments, std::nullopt, Redirects, 10))
    return std::nullopt;
  auto Contents = llvm::MemoryBuffer::getFile(Output);
  if (!Contents)
    return std::nullopt;
  return (*Contents)->getBuffer().trim().str();
}

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
  if (const auto *Array = Context.getAsConstantArrayType(Type)) {
    auto Result = ConvertType(Context, Array->getElementType());
    if (!Result || Array->getSize().getActiveBits() > 64)
      return std::nullopt;
    Result->AddArray(Array->getSize().getZExtValue());
    Result->CSpelling = Spelling;
    return Result;
  }
  if (const auto *Enum = Type->getAs<clang::EnumType>()) {
    const auto Integer = Enum->getDecl()->getIntegerType();
    if (Integer.isNull())
      return std::nullopt;
    auto Result = ConvertType(Context, Integer);
    if (Result)
      Result->CSpelling = Spelling;
    return Result;
  }
  if (const auto *Record = Type->getAsRecordDecl()) {
    sema::Type Result{K::CRecord, {}};
    Result.CName = Record->getNameAsString();
    if (Result.CName.empty())
      if (const auto *Alias = Type->getAs<clang::TypedefType>())
        Result.CName = Alias->getDecl()->getNameAsString();
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
  llvm::StringSet<> SeenConstants;
  const bool HasResourceDir =
      std::any_of(Arguments.begin(), Arguments.end(), [](const auto &Argument) {
        return Argument.starts_with("-resource-dir");
      });
  const auto ResourceDir =
      HasResourceDir ? std::nullopt : FindClangResourceDir();
  const auto AddType = [&](sema::ExternalType Imported) {
    if (SeenTypes.insert(Imported.Name).second) {
      Result.Types.push_back(std::move(Imported));
      return;
    }
    if (!Imported.Fields.empty())
      for (auto &Existing : Result.Types)
        if (Existing.Name == Imported.Name && Existing.Fields.empty()) {
          Existing = std::move(Imported);
          break;
        }
  };
  for (const auto &Header : Headers) {
    auto Buffer = llvm::MemoryBuffer::getFile(Header);
    if (!Buffer) {
      Result.Diagnostics.push_back("cannot read C header: " + Header);
      continue;
    }
    std::vector<std::string> Args{"-x", "c", "-std=c17"};
    if (ResourceDir)
      Args.push_back("-resource-dir=" + *ResourceDir);
    Args.insert(Args.end(), Arguments.begin(), Arguments.end());
    auto Ast = clang::tooling::buildASTFromCodeWithArgs((*Buffer)->getBuffer(),
                                                        Args, Header, "clang");
    if (!Ast || Ast->getDiagnostics().hasErrorOccurred()) {
      Result.Diagnostics.push_back("Clang could not parse C header: " + Header);
      continue;
    }

    auto &Context = Ast->getASTContext();
    const auto &Sources = Context.getSourceManager();
    const auto Directory = std::filesystem::path(Header).parent_path();
    const auto InImportedHeaders = [&](const clang::Decl *Declaration) {
      const auto Location = Sources.getExpansionLoc(Declaration->getLocation());
      if (Sources.isWrittenInMainFile(Location))
        return true;
      const auto File = Sources.getFilename(Location);
      if (File.empty())
        return false;
      if (!Sources.isInSystemHeader(Location))
        return true;
      const auto Path = std::filesystem::path(File.str()).lexically_normal();
      const auto Relative = Path.lexically_relative(Directory);
      return !Relative.empty() && *Relative.begin() != ".." &&
             !Relative.is_absolute();
    };
    const auto RecordFields = [&](const clang::RecordDecl *Record) {
      std::vector<sema::ExternalType::Field> Fields;
      if (!Record || !Record->getDefinition())
        return Fields;
      Record = Record->getDefinition();
      std::function<void(const clang::RecordDecl *, std::uint64_t)> AddFields =
          [&](const clang::RecordDecl *Owner, std::uint64_t Base) {
            const auto &Layout = Context.getASTRecordLayout(Owner);
            for (const auto *Field : Owner->fields()) {
              const auto Offset =
                  Base + Layout.getFieldOffset(Field->getFieldIndex());
              if (Field->isAnonymousStructOrUnion()) {
                if (const auto *Nested = Field->getType()->getAsRecordDecl())
                  AddFields(Nested, Offset);
                continue;
              }
              if (Field->getName().empty())
                continue;
              auto Value = ConvertType(Context, Field->getType());
              const bool Addressable =
                  Value && !Field->isBitField() && Offset % 8 == 0 &&
                  (Offset / 8) % std::max(1u, sema::GetAlignment(*Value)) == 0;
              Fields.push_back(
                  {Field->getNameAsString(),
                   Value.value_or(sema::Type{sema::BuiltinType::Void, {}}),
                   Offset, Addressable});
            }
          };
      AddFields(Record, 0);
      return Fields;
    };
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Typedef = llvm::dyn_cast<clang::TypedefNameDecl>(Declaration);
      if (!Typedef || !InImportedHeaders(Typedef))
        continue;
      auto Type = ConvertType(Context, Typedef->getUnderlyingType());
      if (!Type || Type->IsVoid())
        continue;
      if (Type->Element == sema::BuiltinType::CRecord && Type->CName.empty())
        Type->CName = Typedef->getNameAsString();
      Type->CSpelling = Typedef->getNameAsString();
      const auto Name = "c." + Typedef->getNameAsString();
      AddType(sema::ExternalType{
          Name, std::move(*Type),
          RecordFields(Typedef->getUnderlyingType()->getAsRecordDecl())});
    }
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Record = llvm::dyn_cast<clang::RecordDecl>(Declaration);
      if (!Record || Record->getName().empty() ||
          !Record->isCompleteDefinition() || !InImportedHeaders(Record))
        continue;
      auto Type = ConvertType(Context, Context.getCanonicalTagType(Record));
      const auto Name = "c." + Record->getNameAsString();
      if (Type)
        AddType(
            sema::ExternalType{Name, std::move(*Type), RecordFields(Record)});
    }
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Enum = llvm::dyn_cast<clang::EnumDecl>(Declaration);
      if (!Enum || !Enum->isCompleteDefinition() || !InImportedHeaders(Enum))
        continue;
      if (!Enum->getName().empty()) {
        auto Type = ConvertType(Context, Context.getCanonicalTagType(Enum));
        const auto Name = "c." + Enum->getNameAsString();
        if (Type)
          AddType({Name, std::move(*Type), {}});
      }
      for (const auto *Item : Enum->enumerators()) {
        const auto Name = "c." + Item->getNameAsString();
        if (!SeenConstants.insert(Name).second)
          continue;
        auto Type = ConvertType(Context, Item->getType());
        if (!Type)
          continue;
        llvm::SmallString<32> Integer;
        Item->getInitVal().toString(Integer, 10);
        Result.Constants.push_back({Name, *Type, std::string(Integer)});
      }
    }
    for (const auto *Declaration : Context.getTranslationUnitDecl()->decls()) {
      const auto *Function = llvm::dyn_cast<clang::FunctionDecl>(Declaration);
      if (!Function || Function->getIdentifier() == nullptr ||
          !InImportedHeaders(Function) || !Function->hasExternalFormalLinkage())
        continue;
      const auto Name = Function->getNameAsString();
      if (!Seen.insert(Name).second)
        continue;
      auto Return = ConvertType(Context, Function->getReturnType());
      if (!Return) {
        if (Sources.isWrittenInMainFile(Function->getLocation()))
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
          if (Sources.isWrittenInMainFile(Function->getLocation()))
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
