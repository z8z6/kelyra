#include "Front/Sema/Sema.h"
#include <algorithm>

using namespace kelyra;

void sema::ReflectionDatabase::Clear() {
  Records.clear();
  Nodes.clear();
}

sema::MetaId sema::ReflectionDatabase::Add(const lex::Node *Node, MetaDeclaration Declaration) {
  Declaration.Id = Records.size();
  const auto Id = Declaration.Id;
  Records.push_back(std::move(Declaration));
  if (Node)
    Nodes.emplace(Node, Id);
  return Id;
}

void sema::ReflectionDatabase::SetAnnotations(const lex::Node &Node,
                                              const std::vector<AnnotationInstance> &Annotations) {
  const auto Id = GetId(Node);
  if (Id)
    Records[*Id].Annotations = Annotations;
}

std::optional<sema::MetaId> sema::ReflectionDatabase::GetId(const lex::Node &Node) const {
  const auto It = Nodes.find(&Node);
  return It == Nodes.end() ? std::nullopt : std::optional<MetaId>(It->second);
}

std::optional<sema::MetaId> sema::ReflectionDatabase::Find(std::string_view QualifiedName,
                                                           MetaKind Kind) const {
  std::optional<MetaId> Match;
  for (const auto &Record : Records)
    if (Record.Kind == Kind && Record.QualifiedName == QualifiedName) {
      if (Match)
        return std::nullopt;
      Match = Record.Id;
    }
  return Match;
}

void sema::Sema::BuildRuntimeReflection(const std::vector<ModuleInput> &Modules) {
  using K = lex::NodeKind;
  for (const auto &[Node, Instances] : AnnotationInstances)
    Reflection.SetAnnotations(*Node, Instances);

  const auto HasReflectAnnotation = [this](const lex::Node &Node) {
    const auto &Annotations = GetAnnotations(Node);
    return std::any_of(
        Annotations.begin(), Annotations.end(), [](const AnnotationInstance &Annotation) {
          return Annotation.Name == "std.annotation.reflect";
        });
  };
  for (const auto &Input : Modules) {
    for (const auto &Child : Input.Ast->children) {
      if (Child->kind != K::ast_class)
        continue;
      const auto ClassId = Reflection.GetId(*Child);
      if (!ClassId)
        continue;
      const bool ReflectClass = HasReflectAnnotation(*Child);
      bool HasReflectedField = false;
      for (const auto &Member : Child->children) {
        if (Member->kind != K::ast_field)
          continue;
        const auto FieldId = Reflection.GetId(*Member);
        if (!FieldId)
          continue;
        auto &Field = Reflection.Records[*FieldId];
        Field.RuntimeReflected = (ReflectClass && Field.Public) || HasReflectAnnotation(*Member);
        HasReflectedField |= Field.RuntimeReflected;
      }
      Reflection.Records[*ClassId].RuntimeReflected = ReflectClass || HasReflectedField;
    }
  }
}
