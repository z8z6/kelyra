#pragma once

#include "Front/Module/Module.h"
#include "Front/Sema/ConstEval.h"
#include "Front/Sema/Type.h"
#include <unordered_map>

namespace kelyra::expansion {
using K = lex::NodeKind;
inline void SetGeneratedLocation(lex::Node &Node, const lex::Location &Loc) {
  Node.Loc = Loc;
  for (auto &Child : Node.children)
    SetGeneratedLocation(*Child, Loc);
}

inline std::string TypeName(const lex::Node &Node) {
  if (Node.kind == K::ast_type)
    return Node.text;
  if (Node.kind == K::ast_pointer_type && Node.children.size() == 1)
    return "*" + TypeName(*Node.children.front());
  if (Node.kind == K::ast_array_type && Node.children.size() == 2) {
    sema::ConstEvaluator Evaluator;
    const auto Length = Evaluator.Evaluate(*Node.children.front());
    return "[" +
           (Length && Length->Type == sema::ConstValue::Kind::Integer ? Length->Text : Node.text) +
           "]" + TypeName(*Node.children.back());
  }
  if (Node.kind == K::ast_slice_type && Node.children.size() == 1)
    return "[]" + std::string(Node.text == "const" ? "const " : "") +
           TypeName(*Node.children.front());
  if (Node.kind == K::ast_function_type && !Node.children.empty()) {
    std::string Name = "fn(";
    for (std::size_t I = 0; I + 1 < Node.children.size(); ++I) {
      if (I)
        Name += ", ";
      Name += TypeName(*Node.children[I]);
    }
    return Name + ") -> " + TypeName(*Node.children.back());
  }
  if (Node.kind != K::ast_generic_type)
    return {};
  std::string Name = Node.text + "<";
  for (const auto &Child : Node.children) {
    if (Name.back() != '<')
      Name += ',';
    Name += TypeName(*Child);
  }
  return Name + ">";
}

inline std::string CalleeName(const lex::Node &Node) {
  if (Node.kind == K::ast_name)
    return Node.text;
  if (Node.kind == K::ast_member && Node.children.size() == 1)
    return CalleeName(*Node.children.front()) + "." + Node.text;
  return {};
}

inline void Substitute(std::unique_ptr<lex::Node> &Node,
                       const std::unordered_map<std::string, const lex::Node *> &Bindings) {
  if (Node->kind == K::ast_call && !Node->children.empty() &&
      Node->children.front()->kind == K::ast_name) {
    const auto Found = Bindings.find(Node->children.front()->text);
    if (Found != Bindings.end()) {
      if (Found->second->kind == K::ast_type) {
        Node->children.front()->text = Found->second->text;
      } else if (Found->second->kind == K::ast_generic_type) {
        auto Apply = std::make_unique<lex::Node>();
        Apply->kind = K::ast_generic_apply;
        Apply->Loc = Node->children.front()->Loc;
        auto Callee = Clone(*Node->children.front());
        Callee->text = Found->second->text;
        Apply->children.push_back(std::move(Callee));
        for (const auto &Argument : Found->second->children)
          Apply->children.push_back(Clone(*Argument));
        Node->children.front() = std::move(Apply);
      }
    }
  }
  if (Node->kind == K::ast_type) {
    const auto Found = Bindings.find(Node->text);
    if (Found != Bindings.end()) {
      auto Replacement = Clone(*Found->second);
      Replacement->Loc = Node->Loc;
      Node = std::move(Replacement);
      return;
    }
  }
  for (auto &Child : Node->children)
    Substitute(Child, Bindings);
}

inline void BindArgument(lex::Node &Node, std::string_view Module) {
  if (!Node.GenericArgument) {
    Node.GenericArgument = true;
    Node.GenericOriginModule = Module;
    if (Node.kind == K::ast_type && !Module.empty() && Node.text.find('.') == std::string::npos &&
        !sema::ParseBuiltinType(Node.text))
      Node.text = std::string(Module) + "." + Node.text;
  }
  for (auto &Child : Node.children)
    BindArgument(*Child, Module);
}

} // namespace kelyra::expansion
