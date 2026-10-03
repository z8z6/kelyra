#include "Front/AST/AST.h"
#include <iomanip>
#include <sstream>

using namespace kelyra::lex;

std::string kelyra::lex::GetNodeName(NodeKind Kind) {
  switch (Kind) {
#define NODE(Kind, Id, Name)                                                                       \
  case NodeKind::Kind:                                                                             \
    return Name;
#include "Front/AST/Node.def"
#undef NODE
  }
  return "Unknown";
}

bool kelyra::lex::IsValidNodeKind(std::uint32_t Value) {
  switch (static_cast<NodeKind>(Value)) {
#define NODE(Kind, Id, Name)                                                                       \
  case NodeKind::Kind:                                                                             \
    return true;
#include "Front/AST/Node.def"
#undef NODE
  }
  return false;
}

std::ostream &kelyra::lex::operator<<(std::ostream &Stream, NodeKind Kind) {
  return Stream << GetNodeName(Kind);
}

void kelyra::lex::DumpAst(const Node &Node, std::ostream &Stream) {
  Stream << '(' << Node.kind;
  if (!Node.text.empty())
    Stream << ' ' << std::quoted(Node.text);
  for (const auto &Child : Node.children) {
    Stream << ' ';
    DumpAst(*Child, Stream);
  }
  Stream << ')';
}

std::string kelyra::lex::DumpAst(const Node &Node) {
  std::ostringstream Stream;
  DumpAst(Node, Stream);
  return Stream.str();
}

std::unique_ptr<Node> kelyra::lex::Clone(const Node &Source) {
  auto Result = std::make_unique<Node>();
  Result->kind = Source.kind;
  Result->Loc = Source.Loc;
  Result->text = Source.text;
  Result->height = Source.height;
  Result->GenericInstance = Source.GenericInstance;
  Result->GenericArgument = Source.GenericArgument;
  Result->BoundFieldName = Source.BoundFieldName;
  Result->AssociatedOwnerArguments = Source.AssociatedOwnerArguments;
  Result->GenericOriginModule = Source.GenericOriginModule;
  Result->AnnotationOriginModule = Source.AnnotationOriginModule;
  for (const auto &Child : Source.children)
    Result->children.push_back(Clone(*Child));
  return Result;
}

std::string kelyra::lex::GetModuleName(const Node &Root) {
  for (const auto &Child : Root.children)
    if (Child->kind == NodeKind::ast_module_decl)
      return Child->text;
  return {};
}

std::optional<std::string> kelyra::lex::GetQualifiedName(const lex::Node &Node) {
  if (Node.kind == lex::NodeKind::ast_name)
    return Node.text;
  if (Node.kind != lex::NodeKind::ast_member || Node.children.size() != 1)
    return std::nullopt;
  auto Parent = GetQualifiedName(*Node.children.front());
  return Parent ? std::optional<std::string>(*Parent + "." + Node.text) : std::nullopt;
}
