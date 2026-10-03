#pragma once

#include "Front/Lexer/Diagnostic.h"
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kelyra::lex {
enum class NodeKind : std::uint32_t {
#define NODE(Kind, Id, Name) Kind = Id,
#include "Front/AST/Node.def"
#undef NODE
};

std::string GetNodeName(NodeKind Kind);
bool IsValidNodeKind(std::uint32_t Value);
std::ostream &operator<<(std::ostream &Stream, NodeKind Kind);

// Children are in source order. Declarations: annotations first. Annotation
// declarations: parameters with type and optional default. Functions:
// parameters, optional return type, body. Let: name, optional type, optional
// initializer. If/When: condition, then, optional else. Text holds a name,
// operator, literal spelling, or "let" for Let.
struct Node {
  NodeKind kind;
  Location Loc;
  std::string text;
  std::vector<std::unique_ptr<Node>> children;
  std::size_t height = 1;
  bool GenericInstance = false;
  bool GenericArgument = false;
  bool BoundFieldName = false;
  std::size_t AssociatedOwnerArguments = 0;
  std::string GenericOriginModule;
  std::string AnnotationOriginModule;
};

std::unique_ptr<Node> Clone(const Node &Source);
std::string GetModuleName(const Node &Root);
std::optional<std::string> GetQualifiedName(const Node &Node);
void DumpAst(const Node &Node, std::ostream &Stream);
std::string DumpAst(const Node &Node);

inline bool IsExpressionNode(NodeKind Kind) {
  switch (Kind) {
  case NodeKind::ast_literal:
  case NodeKind::ast_name:
  case NodeKind::ast_unary:
  case NodeKind::ast_binary:
  case NodeKind::ast_cast:
  case NodeKind::ast_call:
  case NodeKind::ast_index:
  case NodeKind::ast_slice:
  case NodeKind::ast_member:
  case NodeKind::ast_group:
  case NodeKind::ast_meta:
  case NodeKind::ast_block_expr:
  case NodeKind::ast_meta_block:
  case NodeKind::ast_match:
  case NodeKind::ast_generic_apply:
  case NodeKind::ast_spread:
    return true;
  default:
    return false;
  }
}
} // namespace kelyra::lex
