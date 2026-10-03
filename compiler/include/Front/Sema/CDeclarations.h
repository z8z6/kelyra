#pragma once
#include "Front/Sema/Type.h"
#include <map>
#include <string>
#include <vector>

namespace kelyra::sema {
struct ExternalFunction {
  std::string Name;
  std::string Header;
  std::vector<Type> Parameters;
  Type Return{BuiltinType::CInt, {}};
  unsigned CallingConvention = 0;
  bool Variadic = false;
  bool operator==(const ExternalFunction &Other) const {
    return Name == Other.Name && Parameters == Other.Parameters && Return == Other.Return &&
           CallingConvention == Other.CallingConvention && Variadic == Other.Variadic;
  }
};

struct ExternalType {
  std::string Name;
  Type Value;
  struct Field {
    std::string Name;
    Type Value;
    std::uint64_t OffsetBits = 0;
    bool Addressable = true;
    bool operator==(const Field &) const = default;
  };
  std::vector<Field> Fields;
  bool operator==(const ExternalType &) const = default;
};

struct ExternalConstant {
  std::string Name;
  Type Value;
  std::string Integer;
  bool operator==(const ExternalConstant &) const = default;
};

struct CDeclarations {
  std::vector<ExternalFunction> Functions;
  std::vector<ExternalType> Types;
  std::vector<ExternalConstant> Constants;
  // Preprocessed C declarations for bridges instantiated by a KMI consumer.
  std::map<std::string, std::string> Headers;

  bool Merge(const CDeclarations &Other);
};
} // namespace kelyra::sema
