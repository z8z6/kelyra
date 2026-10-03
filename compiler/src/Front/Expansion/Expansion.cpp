#include "Front/Expansion/Expansion.h"
#include "Front/Expansion/AnnotationExpander.h"
#include "Front/Expansion/GenericExpander.h"

using namespace kelyra;

bool kelyra::ExpandModules(const std::vector<const Module *> &Modules) {
  ExpansionWorklist Work;
  GenericExpander Generics(Modules, Work);
  AnnotationExpander Annotations(
      Modules, Work, [&](std::string_view Name, std::string_view Module) {
        return Generics.IsTemplate(Name, Module);
      });
  // Read annotation definitions before generic templates leave the AST.
  if (!Annotations.Initialize() || !Generics.Initialize())
    return false;
  while (!Work.empty()) {
    const auto [Node, Source] = Work.front();
    Work.pop_front();
    if (Node->kind == lex::NodeKind::ast_annotation_decl)
      continue;
    if (!Annotations.Expand(*Node, *Source) || !Generics.Expand(*Node, *Source))
      return false;
  }
  return Generics.Finish();
}
