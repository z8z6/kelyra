//
// Created by zhou_zhengming on 2026/10/2.
//

#pragma once

#include "Front/Module/Module.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kelyra {

struct ModuleInterfaceHeader;
class ModuleInterface;

// Binary streams: integers are little endian, strings are length-prefixed.
class KMIOutputStream {
  std::ostream &Stream;
  unsigned Depth = 0;
  std::uint32_t RemainingNodes = 0;
  void WriteNode(const lex::Node &Node, bool KeepBodies);

public:
  explicit KMIOutputStream(std::ostream &Stream) : Stream(Stream) {}
  explicit operator bool() const;
  void Fail();
  KMIOutputStream &operator<<(std::uint32_t Value);
  KMIOutputStream &operator<<(std::uint64_t Value);
  KMIOutputStream &operator<<(const sema::Type &Value);
  KMIOutputStream &operator<<(const sema::TypeModifier &Value);
  KMIOutputStream &operator<<(const sema::ExternalFunction &Value);
  KMIOutputStream &operator<<(const sema::ExternalType &Value);
  KMIOutputStream &operator<<(const sema::ExternalType::Field &Value);
  KMIOutputStream &operator<<(const sema::ExternalConstant &Value);
  KMIOutputStream &operator<<(const sema::CDeclarations &Value);
  template <typename T> KMIOutputStream &operator<<(const std::vector<T> &Values) {
    if (Values.size() > 1'000'000) {
      Fail();
      return *this;
    }
    *this << static_cast<std::uint32_t>(Values.size());
    for (const auto &Value : Values)
      *this << Value;
    return *this;
  }
  KMIOutputStream &operator<<(std::string_view Value);
  KMIOutputStream &operator<<(const lex::Node &Node);
  KMIOutputStream &operator<<(const ModuleInterfaceHeader &Header);
  KMIOutputStream &operator<<(const Module &Module);
  KMIOutputStream &operator<<(const ModuleInterface &Interface);
};

class KMIInputStream {
  std::istream &Stream;
  unsigned Depth = 0;
  std::uint32_t RemainingNodes = 0;

public:
  explicit KMIInputStream(std::istream &Stream) : Stream(Stream) {}
  explicit operator bool() const;
  void Fail();
  KMIInputStream &operator>>(std::uint32_t &Value);
  KMIInputStream &operator>>(std::uint64_t &Value);
  KMIInputStream &operator>>(sema::Type &Value);
  KMIInputStream &operator>>(sema::TypeModifier &Value);
  KMIInputStream &operator>>(sema::ExternalFunction &Value);
  KMIInputStream &operator>>(sema::ExternalType &Value);
  KMIInputStream &operator>>(sema::ExternalType::Field &Value);
  KMIInputStream &operator>>(sema::ExternalConstant &Value);
  KMIInputStream &operator>>(sema::CDeclarations &Value);
  template <typename T> KMIInputStream &operator>>(std::vector<T> &Values) {
    std::uint32_t Count = 0;
    if (!(*this >> Count))
      return *this;
    if (Count > 1'000'000) {
      Fail();
      return *this;
    }
    std::vector<T> Result;
    for (std::uint32_t I = 0; I < Count && *this; ++I) {
      T Value{};
      if (*this >> Value)
        Result.push_back(std::move(Value));
    }
    if (*this)
      Values = std::move(Result);
    return *this;
  }
  KMIInputStream &operator>>(std::string &Value);
  KMIInputStream &operator>>(std::unique_ptr<lex::Node> &Node);
  KMIInputStream &operator>>(ModuleInterfaceHeader &Header);
  KMIInputStream &operator>>(Module &Module);
  KMIInputStream &operator>>(ModuleInterface &Interface);
};

struct ModuleInterfaceHeader {
  std::string Format;
  std::string LLVMVersion;
  std::string TargetTriple;
  std::uint32_t ModuleCount = 0;

  bool Check() const;
};

// In-memory representation of a KMI file, serialized through binary streams.
class ModuleInterface {
public:
  std::filesystem::path Path;
  ModuleInterfaceHeader Header;
  std::vector<Module> Modules;
  sema::CDeclarations Declarations;

  explicit ModuleInterface(std::filesystem::path Path);
  bool Load();
  static bool Write(const std::filesystem::path &Path, const std::filesystem::path &SourceRoot,
                    const std::string &TargetTriple, const std::vector<const Module *> &Modules,
                    const sema::CDeclarations &Declarations);

  static constexpr std::string_view Format = "KELYRA-KMI-6";
  static constexpr std::string_view Extension = ".kmi";
  static bool isKMI(const std::filesystem::path &path);
};
// Capture unexpanded declarations, then publish only after compilation succeeds.
class ModuleInterfaceOutput {
  std::string TemporaryPath;
  std::string OutputPath;

public:
  ModuleInterfaceOutput() = default;
  ModuleInterfaceOutput(const ModuleInterfaceOutput &) = delete;
  ModuleInterfaceOutput &operator=(const ModuleInterfaceOutput &) = delete;
  ~ModuleInterfaceOutput();
  bool Prepare(const ModuleLoader &Loader, const sema::CDeclarations &Declarations,
               bool ExclusiveAction = false);
  bool Publish();
};
} // namespace kelyra
