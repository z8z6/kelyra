#include "Front/Module/ModuleInterface.h"
#include "Support/Log.h"
#include "Support/Option.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include "llvm/Config/llvm-config.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <utility>

using namespace kelyra;

namespace {
llvm::cl::opt<std::string> EmitInterface{"emit-interface",
                                         llvm::cl::desc("Write a compiled library interface"),
                                         llvm::cl::cat(Option::KelyraCategory)};

constexpr std::uint32_t MaxNodes = 1'000'000;
constexpr std::uint32_t MaxString = 16 * 1024 * 1024;
constexpr std::uint32_t MaxModules = 100'000;
constexpr unsigned MaxDepth = 512;

bool HasGenericParameters(const lex::Node &Node) {
  for (const auto &Child : Node.children)
    if (Child->kind == lex::NodeKind::ast_generic_parameter ||
        Child->kind == lex::NodeKind::ast_generic_pack)
      return true;
  return false;
}

bool HasMetaAnnotation(const lex::Node &Node) {
  for (const auto &Child : Node.children)
    if (Child->kind == lex::NodeKind::ast_annotation &&
        (Child->text == "meta" || Child->text == "std.annotation.meta"))
      return true;
  return false;
}

bool IsMetaModule(const lex::Node &Root) {
  for (const auto &Child : Root.children)
    if (Child->kind == lex::NodeKind::ast_module_decl)
      return HasMetaAnnotation(*Child);
  return false;
}

bool Within(const std::filesystem::path &Path, const std::filesystem::path &Root) {
  const auto Relative = Path.lexically_normal().lexically_relative(Root);
  return !Relative.empty() && *Relative.begin() != "..";
}
} // namespace

KMIOutputStream::operator bool() const { return static_cast<bool>(Stream); }
KMIInputStream::operator bool() const { return static_cast<bool>(Stream); }
void KMIOutputStream::Fail() { Stream.setstate(std::ios::failbit); }
void KMIInputStream::Fail() { Stream.setstate(std::ios::failbit); }

KMIOutputStream &KMIOutputStream::operator<<(std::uint32_t Value) {
  if (*this)
    for (unsigned I = 0; I < 4; ++I)
      Stream.put(static_cast<char>((Value >> (I * 8)) & 0xff));
  return *this;
}

KMIInputStream &KMIInputStream::operator>>(std::uint32_t &Value) {
  if (!*this)
    return *this;
  std::uint32_t Result = 0;
  for (unsigned I = 0; I < 4; ++I) {
    const int Byte = Stream.get();
    if (Byte == std::char_traits<char>::eof())
      return *this;
    Result |= static_cast<std::uint32_t>(Byte) << (I * 8);
  }
  Value = Result;
  return *this;
}

KMIOutputStream &KMIOutputStream::operator<<(std::string_view Value) {
  if (!*this)
    return *this;
  if (Value.size() > MaxString) {
    Fail();
    return *this;
  }
  *this << static_cast<std::uint32_t>(Value.size());
  Stream.write(Value.data(), static_cast<std::streamsize>(Value.size()));
  return *this;
}

KMIInputStream &KMIInputStream::operator>>(std::string &Value) {
  std::uint32_t Length = 0;
  if (!(*this >> Length))
    return *this;
  if (Length > MaxString) {
    Fail();
    return *this;
  }
  std::string Result(Length, '\0');
  if (Stream.read(Result.data(), Length))
    Value = std::move(Result);
  return *this;
}

KMIOutputStream &KMIOutputStream::operator<<(std::uint64_t Value) {
  return *this << static_cast<std::uint32_t>(Value) << static_cast<std::uint32_t>(Value >> 32);
}
KMIInputStream &KMIInputStream::operator>>(std::uint64_t &Value) {
  std::uint32_t Low = 0, High = 0;
  if (*this >> Low >> High)
    Value = static_cast<std::uint64_t>(Low) | (static_cast<std::uint64_t>(High) << 32);
  return *this;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::TypeModifier &Value) {
  return *this << static_cast<std::uint32_t>(Value.Kind) << Value.Length
               << static_cast<std::uint32_t>(Value.ReadOnly);
}
KMIInputStream &KMIInputStream::operator>>(sema::TypeModifier &Value) {
  std::uint32_t Kind = 0, ReadOnly = 0;
  sema::TypeModifier Result{};
  if (*this >> Kind >> Result.Length >> ReadOnly) {
    if (Kind > static_cast<std::uint32_t>(sema::TypeModifierKind::Slice) || ReadOnly > 1)
      Fail();
    else {
      Result.Kind = static_cast<sema::TypeModifierKind>(Kind);
      Result.ReadOnly = ReadOnly != 0;
      Value = Result;
    }
  }
  return *this;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::Type &Value) {
  if (Depth++ > MaxDepth) {
    --Depth;
    Fail();
    return *this;
  }
  *this << static_cast<std::uint32_t>(Value.Element) << Value.Dimensions << Value.BitWidth
        << Value.Alignment << Value.PointerDepth << Value.CName << Value.CSpelling
        << Value.ClassName << Value.EnumName << static_cast<std::uint32_t>(Value.GenericArgument)
        << Value.GenericOriginModule << Value.Results << Value.Parameters << Value.Modifiers;
  --Depth;
  return *this;
}
KMIInputStream &KMIInputStream::operator>>(sema::Type &Value) {
  if (Depth++ > MaxDepth) {
    --Depth;
    Fail();
    return *this;
  }
  std::uint32_t Element = 0, GenericArgument = 0;
  sema::Type Result{};
  *this >> Element >> Result.Dimensions >> Result.BitWidth >> Result.Alignment >>
      Result.PointerDepth >> Result.CName >> Result.CSpelling >> Result.ClassName >>
      Result.EnumName >> GenericArgument >> Result.GenericOriginModule >> Result.Results >>
      Result.Parameters >> Result.Modifiers;
  --Depth;
  if (*this) {
    if (Element >= static_cast<std::uint32_t>(sema::BuiltinType::Count) || GenericArgument > 1)
      Fail();
    else {
      Result.Element = static_cast<sema::BuiltinType>(Element);
      Result.GenericArgument = GenericArgument != 0;
      Value = std::move(Result);
    }
  }
  return *this;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::ExternalFunction &Value) {
  return *this << Value.Name << Value.Header << Value.Parameters << Value.Return
               << Value.CallingConvention << static_cast<std::uint32_t>(Value.Variadic);
}
KMIInputStream &KMIInputStream::operator>>(sema::ExternalFunction &Value) {
  sema::ExternalFunction Result;
  std::uint32_t Variadic = 0;
  if (*this >> Result.Name >> Result.Header >> Result.Parameters >> Result.Return >>
      Result.CallingConvention >> Variadic) {
    if (Variadic > 1)
      Fail();
    else {
      Result.Variadic = Variadic != 0;
      Value = std::move(Result);
    }
  }
  return *this;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::ExternalType::Field &Value) {
  return *this << Value.Name << Value.Value << Value.OffsetBits
               << static_cast<std::uint32_t>(Value.Addressable);
}
KMIInputStream &KMIInputStream::operator>>(sema::ExternalType::Field &Value) {
  sema::ExternalType::Field Result;
  std::uint32_t Addressable = 0;
  if (*this >> Result.Name >> Result.Value >> Result.OffsetBits >> Addressable) {
    if (Addressable > 1)
      Fail();
    else {
      Result.Addressable = Addressable != 0;
      Value = std::move(Result);
    }
  }
  return *this;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::ExternalType &Value) {
  return *this << Value.Name << Value.Value << Value.Fields;
}
KMIInputStream &KMIInputStream::operator>>(sema::ExternalType &Value) {
  return *this >> Value.Name >> Value.Value >> Value.Fields;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::ExternalConstant &Value) {
  return *this << Value.Name << Value.Value << Value.Integer;
}
KMIInputStream &KMIInputStream::operator>>(sema::ExternalConstant &Value) {
  return *this >> Value.Name >> Value.Value >> Value.Integer;
}
KMIOutputStream &KMIOutputStream::operator<<(const sema::CDeclarations &Value) {
  if (Value.Headers.size() > MaxModules) {
    Fail();
    return *this;
  }
  *this << Value.Functions << Value.Types << Value.Constants
        << static_cast<std::uint32_t>(Value.Headers.size());
  for (const auto &[Name, Source] : Value.Headers)
    *this << Name << Source;
  return *this;
}
KMIInputStream &KMIInputStream::operator>>(sema::CDeclarations &Value) {
  sema::CDeclarations Result;
  std::uint32_t Count = 0;
  if (!(*this >> Result.Functions >> Result.Types >> Result.Constants >> Count))
    return *this;
  if (Count > MaxModules) {
    Fail();
    return *this;
  }
  for (std::uint32_t I = 0; I < Count && *this; ++I) {
    std::string Name, Source;
    if (*this >> Name >> Source)
      if (!Result.Headers.emplace(std::move(Name), std::move(Source)).second)
        Fail();
  }
  if (*this)
    Value = std::move(Result);
  return *this;
}

KMIOutputStream &KMIOutputStream::operator<<(const ModuleInterfaceHeader &Header) {
  return *this << Header.Format << Header.LLVMVersion << Header.TargetTriple << Header.ModuleCount;
}

KMIInputStream &KMIInputStream::operator>>(ModuleInterfaceHeader &Header) {
  ModuleInterfaceHeader Result;
  if (*this >> Result.Format >> Result.LLVMVersion >> Result.TargetTriple >> Result.ModuleCount)
    Header = std::move(Result);
  return *this;
}

void KMIOutputStream::WriteNode(const lex::Node &Node, bool KeepBodies) {
  if (!*this)
    return;
  if (Depth == 0)
    RemainingNodes = MaxNodes;
  if (Depth > MaxDepth || RemainingNodes == 0) {
    Fail();
    return;
  }
  --RemainingNodes;
  using K = lex::NodeKind;
  const bool KeepChildrenBodies =
      KeepBodies || Node.GenericInstance || HasMetaAnnotation(Node) ||
      Node.kind == K::ast_annotation_decl ||
      ((Node.kind == K::ast_class || Node.kind == K::ast_function) && HasGenericParameters(Node));
  const bool StripBody = (Node.kind == K::ast_function || Node.kind == K::ast_constructor ||
                          Node.kind == K::ast_destructor) &&
                         !KeepChildrenBodies;
  std::size_t Children = Node.children.size();
  if (StripBody)
    Children -= std::count_if(Node.children.begin(), Node.children.end(), [](const auto &Child) {
      return Child->kind == K::ast_block;
    });
  for (const auto Value : {Node.Loc.Begin,
                           Node.Loc.Line,
                           Node.Loc.Column,
                           Node.Loc.End,
                           static_cast<std::size_t>(Node.height),
                           Node.AssociatedOwnerArguments,
                           Children})
    if (Value > std::numeric_limits<std::uint32_t>::max()) {
      Fail();
      return;
    }
  *this << static_cast<std::uint32_t>(Node.kind) << static_cast<std::uint32_t>(Node.Loc.Begin)
        << static_cast<std::uint32_t>(Node.Loc.Line) << static_cast<std::uint32_t>(Node.Loc.Column)
        << static_cast<std::uint32_t>(Node.Loc.End) << static_cast<std::uint32_t>(Node.height)
        << static_cast<std::uint32_t>(Node.GenericInstance)
        << static_cast<std::uint32_t>(Node.GenericArgument)
        << static_cast<std::uint32_t>(Node.BoundFieldName)
        << static_cast<std::uint32_t>(Node.AssociatedOwnerArguments) << Node.text
        << Node.GenericOriginModule << Node.AnnotationOriginModule
        << static_cast<std::uint32_t>(Children);
  ++Depth;
  for (const auto &Child : Node.children)
    if (!StripBody || Child->kind != K::ast_block)
      WriteNode(*Child, KeepChildrenBodies);
  --Depth;
}

KMIOutputStream &KMIOutputStream::operator<<(const lex::Node &Node) {
  WriteNode(Node, false);
  return *this;
}

KMIInputStream &KMIInputStream::operator>>(std::unique_ptr<lex::Node> &Node) {
  if (!*this)
    return *this;
  if (Depth == 0)
    RemainingNodes = MaxNodes;
  if (Depth > MaxDepth || RemainingNodes == 0) {
    Fail();
    return *this;
  }
  --RemainingNodes;
  std::uint32_t Kind = 0, Begin = 0, Line = 0, Column = 0, End = 0;
  std::uint32_t Height = 0, Instance = 0, Argument = 0, Bound = 0;
  std::uint32_t OwnerArgs = 0, Children = 0;
  *this >> Kind >> Begin >> Line >> Column >> End >> Height >> Instance >> Argument >> Bound >>
      OwnerArgs;
  if (!*this)
    return *this;
  if (!lex::IsValidNodeKind(Kind) || End < Begin || Instance > 1 || Argument > 1 || Bound > 1) {
    Fail();
    return *this;
  }
  auto Result = std::make_unique<lex::Node>();
  Result->kind = static_cast<lex::NodeKind>(Kind);
  Result->Loc.Begin = Begin;
  Result->Loc.End = End;
  Result->Loc.Line = Line;
  Result->Loc.Column = Column;
  Result->height = Height;
  Result->GenericInstance = Instance != 0;
  Result->GenericArgument = Argument != 0;
  Result->BoundFieldName = Bound != 0;
  Result->AssociatedOwnerArguments = OwnerArgs;
  *this >> Result->text >> Result->GenericOriginModule >> Result->AnnotationOriginModule >>
      Children;
  if (!*this)
    return *this;
  if (Children > RemainingNodes) {
    Fail();
    return *this;
  }
  ++Depth;
  for (std::uint32_t I = 0; I < Children && *this; ++I) {
    std::unique_ptr<lex::Node> Child;
    if (*this >> Child)
      Result->children.push_back(std::move(Child));
  }
  --Depth;
  if (*this)
    Node = std::move(Result);
  return *this;
}

KMIOutputStream &KMIOutputStream::operator<<(const Module &Module) {
  if (!*this)
    return *this;
  if (!Module.Lex.root) {
    Fail();
    return *this;
  }
  *this << Module.Name << (Module.Name + ".kly");
  WriteNode(*Module.Lex.root, IsMetaModule(*Module.Lex.root));
  return *this;
}

KMIInputStream &KMIInputStream::operator>>(Module &Module) {
  kelyra::Module Result;
  std::string SourcePath;
  if (*this >> Result.Name >> SourcePath >> Result.Lex.root) {
    Result.SetPath(SourcePath);
    Result.IsExternal = true;
    Module = std::move(Result);
  }
  return *this;
}

KMIOutputStream &KMIOutputStream::operator<<(const ModuleInterface &Interface) {
  if (!*this)
    return *this;
  if (Interface.Modules.size() > MaxModules) {
    Fail();
    return *this;
  }
  auto Header = Interface.Header;
  Header.ModuleCount = static_cast<std::uint32_t>(Interface.Modules.size());
  *this << Header << Interface.Declarations;
  for (const auto &Module : Interface.Modules)
    *this << Module;
  return *this;
}

KMIInputStream &KMIInputStream::operator>>(ModuleInterface &Interface) {
  ModuleInterfaceHeader Header;
  if (!(*this >> Header))
    return *this;
  if (!Header.Check()) {
    Fail();
    return *this;
  }
  sema::CDeclarations Declarations;
  if (!(*this >> Declarations))
    return *this;
  std::vector<Module> Modules;
  for (std::uint32_t I = 0; I < Header.ModuleCount && *this; ++I) {
    Module Module;
    if (*this >> Module)
      Modules.push_back(std::move(Module));
  }
  if (*this) {
    Interface.Header = std::move(Header);
    Interface.Modules = std::move(Modules);
    Interface.Declarations = std::move(Declarations);
  }
  return *this;
}

bool ModuleInterfaceHeader::Check() const {
  return Format == ModuleInterface::Format && LLVMVersion == LLVM_VERSION_STRING &&
         TargetTriple == Option::Target.getValue() && ModuleCount <= MaxModules;
}

ModuleInterface::ModuleInterface(std::filesystem::path Path) : Path(std::move(Path)) {}

bool ModuleInterface::Load() {
  std::ifstream File(Path, std::ios::binary);
  KMIInputStream In(File);
  return static_cast<bool>(In >> *this);
}

bool ModuleInterface::isKMI(const std::filesystem::path &Path) {
  return Path.extension() == Extension;
}

bool ModuleInterface::Write(const std::filesystem::path &Path,
                            const std::filesystem::path &SourceRoot,
                            const std::string &TargetTriple,
                            const std::vector<const Module *> &Modules,
                            const sema::CDeclarations &Declarations) {
  std::vector<const Module *> Owned;
  const auto Root = std::filesystem::absolute(SourceRoot).lexically_normal();
  for (const auto *Module : Modules)
    if (!Module->IsExternal && Within(Module->Path, Root) && !Module->Name.empty())
      Owned.push_back(Module);
  if (Owned.empty() || Owned.size() > MaxModules) {
    kerr() << "invalid library module count under " << Root.string() << '\n';
    return false;
  }
  ModuleInterfaceHeader Header;
  Header.Format = Format;
  Header.LLVMVersion = LLVM_VERSION_STRING;
  Header.TargetTriple = TargetTriple;
  Header.ModuleCount = static_cast<std::uint32_t>(Owned.size());
  std::ofstream File(Path, std::ios::binary | std::ios::trunc);
  KMIOutputStream Out(File);
  Out << Header << Declarations;
  for (const auto *Module : Owned)
    Out << *Module;
  File.flush();
  if (!Out) {
    kerr() << "failed to write module interface: " << Path.string() << '\n';
    return false;
  }
  return true;
}

ModuleInterfaceOutput::~ModuleInterfaceOutput() {
  if (!TemporaryPath.empty())
    llvm::sys::fs::remove(TemporaryPath);
}
bool ModuleInterfaceOutput::Prepare(const ModuleLoader &Loader,
                                    const sema::CDeclarations &Declarations, bool ExclusiveAction) {
  if (EmitInterface.empty())
    return true;
  if (ExclusiveAction) {
    kerr() << "--emit-interface cannot be combined with another action\n";
    return false;
  }
  if (!Loader.IsLibrary()) {
    kerr() << "--emit-interface requires --library-root\n";
    return false;
  }
  OutputPath = EmitInterface.getValue();
  llvm::SmallString<128> Path;
  if (auto Error = llvm::sys::fs::createUniqueFile(OutputPath + ".tmp-%%%%%%%%", Path)) {
    kerr() << "cannot create module interface: " << Error.message() << '\n';
    return false;
  }
  TemporaryPath = Path.str().str();
  return ModuleInterface::Write(TemporaryPath,
                                Loader.GetSourceRoot(),
                                Option::Target.getValue(),
                                Loader.GetModules(),
                                Declarations);
}
bool ModuleInterfaceOutput::Publish() {
  if (TemporaryPath.empty())
    return true;
  if (auto Error = llvm::sys::fs::rename(TemporaryPath, OutputPath)) {
    kerr() << "cannot publish module interface: " << Error.message() << '\n';
    return false;
  }
  TemporaryPath.clear();
  return true;
}
