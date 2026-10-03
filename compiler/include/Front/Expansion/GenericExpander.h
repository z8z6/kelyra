#pragma once

#include "Front/Expansion/Expansion.h"
#include <memory>
#include <string_view>

namespace kelyra {
class GenericExpander {
  class Implementation;
  std::unique_ptr<Implementation> Impl;

public:
  GenericExpander(const std::vector<const Module *> &Modules, ExpansionWorklist &Work);
  ~GenericExpander();
  bool Initialize();
  bool Expand(lex::Node &Node, const Module &Source);
  bool IsTemplate(std::string_view Name, std::string_view Module);
  bool Finish();
};
} // namespace kelyra
