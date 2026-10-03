#pragma once

#include "Front/Expansion/Expansion.h"
#include <functional>
#include <memory>
#include <string_view>

namespace kelyra {
class AnnotationExpander {
  class Implementation;
  std::unique_ptr<Implementation> Impl;

public:
  AnnotationExpander(const std::vector<const Module *> &Modules, ExpansionWorklist &Work,
                     std::function<bool(std::string_view, std::string_view)> IsGeneric);
  ~AnnotationExpander();
  bool Initialize();
  bool Expand(lex::Node &Node, const Module &Source);
};
} // namespace kelyra
