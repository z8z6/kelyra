#include "Sema/Reflection.h"

using namespace kelyra;

void sema::ReflectionDatabase::Clear() {
  Records.clear();
  Nodes.clear();
}

sema::MetaId sema::ReflectionDatabase::Add(const lex::Node *Node,
                                           MetaDeclaration Declaration) {
  Declaration.Id = Records.size();
  const auto Id = Declaration.Id;
  Records.push_back(std::move(Declaration));
  if (Node)
    Nodes.emplace(Node, Id);
  return Id;
}

void sema::ReflectionDatabase::SetAnnotations(
    const lex::Node &Node, const std::vector<AnnotationInstance> &Annotations) {
  const auto Id = GetId(Node);
  if (Id)
    Records[*Id].Annotations = Annotations;
}

std::optional<sema::MetaId>
sema::ReflectionDatabase::GetId(const lex::Node &Node) const {
  const auto It = Nodes.find(&Node);
  return It == Nodes.end() ? std::nullopt : std::optional<MetaId>(It->second);
}

std::optional<sema::MetaId>
sema::ReflectionDatabase::Find(std::string_view QualifiedName,
                               MetaKind Kind) const {
  for (const auto &Record : Records)
    if (Record.Kind == Kind && Record.QualifiedName == QualifiedName)
      return Record.Id;
  return std::nullopt;
}
