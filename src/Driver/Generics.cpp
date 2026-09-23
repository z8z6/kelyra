#include "Driver/Generics.h"

#include "Sema/Type.h"
#include "Support/Log.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace kelyra;

namespace {
using K = lex::TokenKind;

std::string ModuleName(const lex::Node &Root) {
  for (const auto &Child : Root.children)
    if (Child->kind == K::ast_module_decl)
      return Child->text;
  return {};
}

std::unique_ptr<lex::Node> Clone(const lex::Node &Source) {
  auto Result = std::make_unique<lex::Node>();
  Result->kind = Source.kind;
  Result->Loc = Source.Loc;
  Result->text = Source.text;
  Result->height = Source.height;
  Result->GenericInstance = Source.GenericInstance;
  Result->GenericArgument = Source.GenericArgument;
  Result->GenericOriginModule = Source.GenericOriginModule;
  for (const auto &Child : Source.children)
    Result->children.push_back(Clone(*Child));
  return Result;
}

std::string TypeName(const lex::Node &Node) {
  if (Node.kind == K::ast_type)
    return Node.text;
  if (Node.kind == K::ast_pointer_type && Node.children.size() == 1)
    return "*" + TypeName(*Node.children.front());
  if (Node.kind == K::ast_array_type && Node.children.size() == 1)
    return "[" + Node.text + "]" + TypeName(*Node.children.front());
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

std::string CalleeName(const lex::Node &Node) {
  if (Node.kind == K::ast_name)
    return Node.text;
  if (Node.kind == K::ast_member && Node.children.size() == 1)
    return CalleeName(*Node.children.front()) + "." + Node.text;
  return {};
}

std::string Encode(std::string_view Name) {
  constexpr char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Name.size() * 2);
  for (const unsigned char Byte : Name) {
    Result.push_back(Digits[Byte >> 4]);
    Result.push_back(Digits[Byte & 15]);
  }
  return Result;
}

struct Template {
  std::unique_ptr<lex::Node> Declaration;
  SourceModule *Module = nullptr;
  std::string ModuleName;
  std::vector<std::string> Parameters;
};

class GenericExpander {
  std::deque<SourceModule> &Modules;
  std::map<std::string, Template> Templates;
  std::unordered_map<std::string, std::string> Instances;
  std::deque<std::pair<lex::Node *, std::string>> Work;
  bool Valid = true;

  void Error(const lex::Location &Loc, std::string_view Message) {
    kerr() << Loc.File << ':' << Loc.Line << ':' << Loc.Column
           << ": error: " << Message << '\n';
    Valid = false;
  }

  const Template *FindTemplate(std::string_view Name,
                               std::string_view Module) const {
    const auto Key = Name.find('.') == std::string_view::npos
                         ? std::string(Module) + "." + std::string(Name)
                         : std::string(Name);
    const auto Found = Templates.find(Key);
    if (Found != Templates.end())
      return &Found->second;
    if (!Module.empty() || Name.find('.') != std::string_view::npos)
      return nullptr;
    const auto Global = Templates.find(std::string(Name));
    return Global == Templates.end() ? nullptr : &Global->second;
  }

  static void Substitute(
      std::unique_ptr<lex::Node> &Node,
      const std::unordered_map<std::string, const lex::Node *> &Bindings) {
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

  static void BindArgument(lex::Node &Node, std::string_view Module) {
    if (!Node.GenericArgument) {
      Node.GenericArgument = true;
      Node.GenericOriginModule = Module;
      if (Node.kind == K::ast_type && !Module.empty() &&
          Node.text.find('.') == std::string::npos &&
          !sema::ParseBuiltinType(Node.text))
        Node.text = std::string(Module) + "." + Node.text;
    }
    for (auto &Child : Node.children)
      BindArgument(*Child, Module);
  }

  std::string Instantiate(const Template &Source,
                          const std::vector<const lex::Node *> &Arguments,
                          const lex::Location &Loc,
                          std::string_view CallerModule) {
    if (Arguments.size() != Source.Parameters.size()) {
      Error(Loc, "wrong number of generic type arguments");
      return {};
    }
    std::vector<std::unique_ptr<lex::Node>> BoundArguments;
    BoundArguments.reserve(Arguments.size());
    std::string Signature;
    for (const auto *Argument : Arguments) {
      auto Bound = Clone(*Argument);
      BindArgument(*Bound, CallerModule);
      const auto Name = TypeName(*Bound);
      if (Name.empty()) {
        Error(Argument->Loc, "unsupported generic type argument");
        return {};
      }
      Signature += std::to_string(Name.size()) + "_" + Name;
      BoundArguments.push_back(std::move(Bound));
    }
    const std::string Key = Source.ModuleName + "." + Source.Declaration->text +
                            "<" + Signature + ">";
    if (const auto Existing = Instances.find(Key); Existing != Instances.end())
      return Existing->second;
    if (Instances.size() >= 256) {
      Error(Loc, "generic instantiation limit exceeded");
      return {};
    }
    const std::string Name =
        Source.Declaration->text + "__G" + Encode(Signature);
    Instances.emplace(Key, Name);
    auto Declaration = Clone(*Source.Declaration);
    Declaration->text = Name;
    Declaration->GenericInstance = true;
    std::unordered_map<std::string, const lex::Node *> Bindings;
    for (std::size_t I = 0; I < Arguments.size(); ++I)
      Bindings.emplace(Source.Parameters[I], BoundArguments[I].get());
    std::erase_if(Declaration->children, [](const auto &Child) {
      return Child->kind == K::ast_generic_parameter;
    });
    for (auto &Child : Declaration->children)
      Substitute(Child, Bindings);
    auto *Generated = Declaration.get();
    Source.Module->Parsed.root->children.push_back(std::move(Declaration));
    Work.emplace_back(Generated, Source.ModuleName);
    return Name;
  }

  void Lower(std::unique_ptr<lex::Node> &Node, std::string_view Module) {
    for (auto &Child : Node->children)
      Lower(Child, Module);
    if (Node->kind != K::ast_generic_type && Node->kind != K::ast_generic_apply)
      return;
    const bool Apply = Node->kind == K::ast_generic_apply;
    lex::Node *Callee = Apply ? Node->children.front().get() : nullptr;
    const auto Name = Apply ? CalleeName(*Callee) : Node->text;
    const auto *Source = FindTemplate(Name, Module);
    if (!Source) {
      Error(Node->Loc, "unknown generic class or function");
      return;
    }
    if ((Apply && Source->Declaration->kind != K::ast_function &&
         Source->Declaration->kind != K::ast_class) ||
        (!Apply && Source->Declaration->kind != K::ast_class)) {
      Error(Node->Loc, "invalid generic use");
      return;
    }
    std::vector<const lex::Node *> Arguments;
    for (std::size_t I = Apply ? 1 : 0; I < Node->children.size(); ++I)
      Arguments.push_back(Node->children[I].get());
    const auto Concrete = Instantiate(*Source, Arguments, Node->Loc, Module);
    if (Concrete.empty())
      return;
    if (Apply) {
      auto Replacement = std::move(Node->children.front());
      Replacement->text = Concrete;
      Replacement->Loc.Len = Node->Loc.End() - Replacement->Loc.Offset;
      Node = std::move(Replacement);
    } else {
      Node->kind = K::ast_type;
      Node->text = Name.find('.') == std::string::npos
                       ? Concrete
                       : Source->ModuleName + "." + Concrete;
      Node->children.clear();
    }
  }

public:
  explicit GenericExpander(std::deque<SourceModule> &Modules)
      : Modules(Modules) {}

  bool Run() {
    for (auto &Module : Modules) {
      const auto Name = ModuleName(*Module.Parsed.root);
      auto &Declarations = Module.Parsed.root->children;
      for (auto It = Declarations.begin(); It != Declarations.end();) {
        std::vector<std::string> Parameters;
        for (const auto &Child : (*It)->children)
          if (Child->kind == K::ast_generic_parameter)
            Parameters.push_back(Child->text);
        if (Parameters.empty()) {
          Work.emplace_back(It->get(), Name);
          ++It;
          continue;
        }
        std::unordered_set<std::string> Seen;
        for (const auto &Parameter : Parameters)
          if (!Seen.insert(Parameter).second)
            Error((*It)->Loc, "duplicate generic type parameter");
        const auto Key = Name.empty() ? (*It)->text : Name + "." + (*It)->text;
        const auto Loc = (*It)->Loc;
        Template Source{std::move(*It), &Module, Name, std::move(Parameters)};
        if (!Templates.emplace(Key, std::move(Source)).second)
          Error(Loc, "duplicate generic declaration");
        It = Declarations.erase(It);
      }
    }
    while (!Work.empty() && Valid) {
      const auto [Node, Module] = Work.front();
      Work.pop_front();
      for (auto &Child : Node->children)
        Lower(Child, Module);
    }
    return Valid;
  }
};
} // namespace

bool kelyra::ExpandGenerics(std::deque<SourceModule> &Modules) {
  return GenericExpander(Modules).Run();
}
