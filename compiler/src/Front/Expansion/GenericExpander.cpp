#include "Front/Expansion/GenericExpander.h"
#include "ExpansionInternal.h"

#include "Front/Sema/Type.h"
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
using namespace kelyra::expansion;

namespace {
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
  const kelyra::Module *Module = nullptr;
  std::string ModuleName;
  std::vector<std::string> Parameters;
  bool Pack = false;
};

} // namespace

class kelyra::GenericExpander::Implementation {
  const std::vector<const Module *> &Modules;
  std::map<std::string, Template> Templates;
  std::vector<std::unordered_map<std::string, const lex::Node *>> AliasScopes;
  std::vector<const lex::Node *> ExpandingAliases;
  std::unordered_map<std::string, std::string> Instances;
  ExpansionWorklist &Work;
  std::unordered_map<lex::Node *, std::vector<std::unique_ptr<lex::Node>>> PendingAliases;
  bool Valid = true;

  void Error(const lex::Location &Loc, std::string_view Message) {
    kerr() << Loc.File << ':' << Loc.Line << ':' << Loc.Column << ": error: " << Message << '\n';
    Valid = false;
  }

  void RegisterScopedAlias(const lex::Node &Alias) {
    if (sema::ParseBuiltinType(Alias.text) ||
        !AliasScopes.back().emplace(Alias.text, &Alias).second) {
      Error(Alias.Loc, "duplicate or reserved alias declaration");
      return;
    }
    std::unordered_set<std::string> Parameters;
    for (const auto &Part : Alias.children)
      if (Part->kind == K::ast_generic_parameter && !Parameters.insert(Part->text).second)
        Error(Part->Loc, "duplicate generic type parameter");
  }

  const Template *FindTemplate(std::string_view Name, std::string_view Module,
                               const lex::Location *Loc = nullptr) {
    const auto Key = Name.find('.') == std::string_view::npos
                         ? std::string(Module) + "." + std::string(Name)
                         : std::string(Name);
    const auto Found = Templates.find(Key);
    if (Found != Templates.end())
      return &Found->second;
    if (Name.find('.') != std::string_view::npos)
      return nullptr;
    const Template *Imported = nullptr;
    for (const auto *Input : Modules) {
      if (Input->Name != Module)
        continue;
      for (const auto &Child : Input->Lex.root->children) {
        if (Child->kind != K::ast_import)
          continue;
        const auto Candidate = Templates.find(Child->text + "." + std::string(Name));
        if (Candidate == Templates.end() ||
            std::none_of(Candidate->second.Declaration->children.begin(),
                         Candidate->second.Declaration->children.end(),
                         [](const auto &Part) { return Part->kind == K::ast_public; }))
          continue;
        if (Imported && Imported != &Candidate->second) {
          if (Loc)
            Error(*Loc, "ambiguous generic name");
          return nullptr;
        }
        Imported = &Candidate->second;
      }
    }
    if (Imported)
      return Imported;
    if (!Module.empty())
      return nullptr;
    const auto Global = Templates.find(std::string(Name));
    return Global == Templates.end() ? nullptr : &Global->second;
  }

  bool ExpandLocalAliases(std::unique_ptr<lex::Node> &Node,
                          std::unordered_set<const lex::Node *> &Visiting) {
    if (Node->kind == K::ast_type && Node->text.find('.') == std::string::npos)
      for (auto Scope = AliasScopes.rbegin(); Scope != AliasScopes.rend(); ++Scope)
        if (const auto Found = Scope->find(Node->text); Found != Scope->end()) {
          const auto *Alias = Found->second;
          if (std::any_of(Alias->children.begin(), Alias->children.end(), [](const auto &Part) {
                return Part->kind == K::ast_generic_parameter;
              }))
            break;
          if (!Visiting.insert(Alias).second) {
            Error(Node->Loc, "recursive local alias");
            return false;
          }
          Node = Clone(*Alias->children.back());
          const bool Valid = ExpandLocalAliases(Node, Visiting);
          Visiting.erase(Alias);
          return Valid;
        }
    for (auto &Child : Node->children)
      if (!ExpandLocalAliases(Child, Visiting))
        return false;
    return true;
  }

  static bool ExpandPackUses(std::unique_ptr<lex::Node> &Node, std::string_view PackName,
                             std::size_t Count) {
    if (Node->kind == K::ast_name && Node->text == PackName)
      return false;
    for (auto &Child : Node->children) {
      if (Child->kind != K::ast_call && !ExpandPackUses(Child, PackName, Count))
        return false;
      if (Child->kind != K::ast_call)
        continue;
      std::vector<std::unique_ptr<lex::Node>> Rewritten;
      for (auto &Argument : Child->children) {
        if (Argument->kind == K::ast_spread) {
          if (Argument->children.size() != 1 || Argument->children.front()->kind != K::ast_name ||
              Argument->children.front()->text != PackName)
            return false;
          for (std::size_t I = 0; I < Count; ++I) {
            auto Name = Clone(*Argument->children.front());
            Name->text = "$pack" + std::to_string(I);
            Rewritten.push_back(std::move(Name));
          }
        } else {
          if (!ExpandPackUses(Argument, PackName, Count))
            return false;
          Rewritten.push_back(std::move(Argument));
        }
      }
      Child->children = std::move(Rewritten);
    }
    return true;
  }

  std::string Instantiate(const Template &Source, const std::vector<const lex::Node *> &Arguments,
                          const lex::Location &Loc, std::string_view CallerModule) {
    if ((!Source.Pack && Arguments.size() != Source.Parameters.size()) ||
        (Source.Pack && Arguments.size() + 1 < Source.Parameters.size())) {
      Error(Loc, "wrong number of generic type arguments");
      return {};
    }
    std::vector<std::unique_ptr<lex::Node>> BoundArguments;
    BoundArguments.reserve(Arguments.size());
    std::string Signature;
    for (const auto *Argument : Arguments) {
      auto Bound = Clone(*Argument);
      std::unordered_set<const lex::Node *> Visiting;
      if (!ExpandLocalAliases(Bound, Visiting))
        return {};
      BindArgument(*Bound, CallerModule);
      const auto Name = TypeName(*Bound);
      if (Name.empty()) {
        Error(Argument->Loc, "unsupported generic type argument");
        return {};
      }
      Signature += std::to_string(Name.size()) + "_" + Name;
      BoundArguments.push_back(std::move(Bound));
    }
    const std::string Key =
        Source.ModuleName + "." + Source.Declaration->text + "<" + Signature + ">";
    if (const auto Existing = Instances.find(Key); Existing != Instances.end())
      return Existing->second;
    if (Instances.size() >= 256) {
      Error(Loc, "generic instantiation limit exceeded");
      return {};
    }
    const std::string Name = Source.Declaration->text + "__G" + Encode(Signature);
    Instances.emplace(Key, Name);
    auto Declaration = Clone(*Source.Declaration);
    Declaration->text = Name;
    Declaration->GenericInstance = true;
    std::unordered_map<std::string, const lex::Node *> Bindings;
    for (std::size_t I = 0; I < Source.Parameters.size() - Source.Pack; ++I)
      Bindings.emplace(Source.Parameters[I], BoundArguments[I].get());
    std::erase_if(Declaration->children, [](const auto &Child) {
      return Child->kind == K::ast_generic_parameter || Child->kind == K::ast_generic_pack;
    });
    for (auto &Child : Declaration->children)
      Substitute(Child, Bindings);
    if (Source.Pack) {
      const auto PackTypeName = Source.Parameters.back();
      const auto Count = Arguments.size() - (Source.Parameters.size() - 1);
      auto Parameter =
          std::find_if(Declaration->children.begin(),
                       Declaration->children.end(),
                       [](const auto &Child) { return Child->kind == K::ast_parameter_pack; });
      if (Parameter == Declaration->children.end() || (*Parameter)->children.empty() ||
          (*Parameter)->children.back()->kind != K::ast_type ||
          (*Parameter)->children.back()->text != PackTypeName ||
          std::any_of(std::next(Parameter), Declaration->children.end(), [](const auto &Child) {
            return Child->kind == K::ast_parameter || Child->kind == K::ast_parameter_pack;
          })) {
        Error(Loc, "generic parameter pack requires a trailing parameter pack");
        return {};
      }
      for (const auto &Child : (*Parameter)->children)
        if (Child->kind == K::ast_annotation &&
            (Child->text == "forward" || Child->text == "std.annotation.forward")) {
          Error(Child->Loc, "@forward on function parameter packs is not supported");
          return {};
        }
      const auto Position = std::distance(Declaration->children.begin(), Parameter);
      const auto PackVariableName = (*Parameter)->text;
      Declaration->children.erase(Parameter);
      for (std::size_t I = 0; I < Count; ++I) {
        auto Concrete = std::make_unique<lex::Node>();
        Concrete->kind = K::ast_parameter;
        Concrete->Loc = Loc;
        Concrete->text = "$pack" + std::to_string(I);
        Concrete->children.push_back(Clone(*BoundArguments[Source.Parameters.size() - 1 + I]));
        Declaration->children.insert(Declaration->children.begin() + Position + I,
                                     std::move(Concrete));
      }
      for (auto &Child : Declaration->children)
        if (Child->kind == K::ast_block && !ExpandPackUses(Child, PackVariableName, Count)) {
          Error(Child->Loc, "invalid parameter pack use");
          return {};
        }
    }
    auto *Generated = Declaration.get();
    Source.Module->Lex.root->children.push_back(std::move(Declaration));
    Work.emplace_back(Generated, Source.Module);
    return Name;
  }

  bool LowerAssociatedAlias(std::unique_ptr<lex::Node> &Node, std::string_view Module,
                            std::string_view Name) {
    const auto Dot = Name.rfind('.');
    if (Dot == std::string_view::npos)
      return false;
    const auto OwnerName = Name.substr(0, Dot);
    const auto AliasName = Name.substr(Dot + 1);
    lex::Node *Owner = nullptr;
    std::string OwnerModule;
    if (const auto *Generic = FindTemplate(OwnerName, Module, &Node->Loc);
        Node->AssociatedOwnerArguments && Generic && Generic->Declaration->kind == K::ast_class) {
      std::vector<const lex::Node *> OwnerArguments;
      for (std::size_t I = 0; I < Node->AssociatedOwnerArguments; ++I)
        OwnerArguments.push_back(Node->children[I].get());
      const auto Concrete = Instantiate(*Generic, OwnerArguments, Node->Loc, Module);
      if (Concrete.empty())
        return true;
      OwnerModule = Generic->ModuleName;
      for (const auto &Part : Generic->Module->Lex.root->children)
        if (Part->kind == K::ast_class && Part->text == Concrete) {
          Owner = Part.get();
          break;
        }
    } else if (!Node->AssociatedOwnerArguments) {
      auto FindClass = [&](std::string_view CandidateModule,
                           std::string_view CandidateName) -> lex::Node * {
        for (const auto *Input : Modules)
          if (Input->Name == CandidateModule)
            for (auto &Part : Input->Lex.root->children)
              if (Part->kind == K::ast_class && Part->text == CandidateName)
                return Part.get();
        return nullptr;
      };
      if (const auto Split = OwnerName.rfind('.'); Split != std::string_view::npos) {
        OwnerModule = std::string(OwnerName.substr(0, Split));
        Owner = FindClass(OwnerModule, OwnerName.substr(Split + 1));
      } else {
        OwnerModule = std::string(Module);
        Owner = FindClass(OwnerModule, OwnerName);
        if (!Owner)
          for (const auto *Input : Modules)
            if (Input->Name == Module)
              for (const auto &Import : Input->Lex.root->children)
                if (Import->kind == K::ast_import) {
                  auto *Candidate = FindClass(Import->text, OwnerName);
                  if (!Candidate ||
                      std::none_of(Candidate->children.begin(),
                                   Candidate->children.end(),
                                   [](const auto &Part) { return Part->kind == K::ast_public; }))
                    continue;
                  if (Owner && Owner != Candidate) {
                    Error(Node->Loc, "ambiguous class name");
                    return true;
                  }
                  Owner = Candidate;
                  OwnerModule = Import->text;
                }
      }
    }
    if (!Owner)
      return false;
    lex::Node *Alias = nullptr;
    for (auto &Part : Owner->children)
      if (Part->kind == K::ast_alias_decl && Part->text == AliasName) {
        Alias = Part.get();
        break;
      }
    if (!Alias)
      return false;
    const bool External = OwnerModule != Module;
    const auto Public = [](const lex::Node &Declaration) {
      return std::any_of(Declaration.children.begin(),
                         Declaration.children.end(),
                         [](const auto &Part) { return Part->kind == K::ast_public; });
    };
    if (External && (!Public(*Owner) || !Public(*Alias))) {
      Error(Node->Loc, "private associated alias");
      return true;
    }
    std::vector<std::string> Parameters;
    for (const auto &Part : Alias->children)
      if (Part->kind == K::ast_generic_parameter)
        Parameters.push_back(Part->text);
    const auto Start = Node->AssociatedOwnerArguments;
    if (Parameters.size() != Node->children.size() - Start) {
      Error(Node->Loc, "wrong number of generic type arguments");
      return true;
    }
    std::vector<std::unique_ptr<lex::Node>> BoundArguments;
    std::string Signature;
    for (std::size_t I = Start; I < Node->children.size(); ++I) {
      auto Bound = Clone(*Node->children[I]);
      std::unordered_set<const lex::Node *> Visiting;
      if (!ExpandLocalAliases(Bound, Visiting))
        return true;
      BindArgument(*Bound, Module);
      const auto Type = TypeName(*Bound);
      if (Type.empty()) {
        Error(Node->Loc, "unsupported generic type argument");
        return true;
      }
      Signature += std::to_string(Type.size()) + "_" + Type;
      BoundArguments.push_back(std::move(Bound));
    }
    const auto Key =
        "member:" + OwnerModule + "." + Owner->text + "." + Alias->text + "<" + Signature + ">";
    auto Existing = Instances.find(Key);
    std::string Concrete;
    if (Existing != Instances.end()) {
      Concrete = Existing->second;
    } else {
      if (Instances.size() >= 256) {
        Error(Node->Loc, "generic instantiation limit exceeded");
        return true;
      }
      Concrete = Alias->text + "__G" + Encode(Signature);
      Instances.emplace(Key, Concrete);
      auto Declaration = Clone(*Alias);
      Declaration->text = Concrete;
      Declaration->GenericInstance = true;
      std::erase_if(Declaration->children,
                    [](const auto &Part) { return Part->kind == K::ast_generic_parameter; });
      std::unordered_map<std::string, const lex::Node *> Bindings;
      for (std::size_t I = 0; I < Parameters.size(); ++I)
        Bindings.emplace(Parameters[I], BoundArguments[I].get());
      for (auto &Part : Declaration->children)
        Substitute(Part, Bindings);
      AliasScopes.emplace_back();
      for (const auto &Part : Owner->children)
        if (Part->kind == K::ast_alias_decl)
          AliasScopes.back().emplace(Part->text, Part.get());
      Lower(Declaration, OwnerModule);
      AliasScopes.pop_back();
      PendingAliases[Owner].push_back(std::move(Declaration));
    }
    Node->kind = K::ast_type;
    Node->text = OwnerModule.empty() ? Owner->text + "." + Concrete
                                     : OwnerModule + "." + Owner->text + "." + Concrete;
    Node->children.clear();
    Node->AssociatedOwnerArguments = 0;
    return true;
  }

  void Lower(std::unique_ptr<lex::Node> &Node, std::string_view Module) {
    const bool Scoped =
        Node->kind == K::ast_block || Node->kind == K::ast_block_expr || Node->kind == K::ast_class;
    if (Scoped) {
      AliasScopes.emplace_back();
      if (Node->kind == K::ast_class)
        for (const auto &Child : Node->children)
          if (Child->kind == K::ast_alias_decl)
            RegisterScopedAlias(*Child);
    }
    for (auto &Child : Node->children) {
      if (Child->kind != K::ast_alias_decl ||
          std::none_of(Child->children.begin(), Child->children.end(), [](const auto &Part) {
            return Part->kind == K::ast_generic_parameter;
          }))
        Lower(Child, Module);
      if (Scoped && Node->kind != K::ast_class && Child->kind == K::ast_alias_decl)
        RegisterScopedAlias(*Child);
    }
    if (Scoped)
      AliasScopes.pop_back();
    if (Node->kind != K::ast_generic_type && Node->kind != K::ast_generic_apply)
      return;
    const bool Apply = Node->kind == K::ast_generic_apply;
    lex::Node *Callee = Apply ? Node->children.front().get() : nullptr;
    const auto Name = Apply ? CalleeName(*Callee) : Node->text;
    if (!Apply && Name.find('.') == std::string::npos)
      for (auto Scope = AliasScopes.rbegin(); Scope != AliasScopes.rend(); ++Scope)
        if (const auto Found = Scope->find(Name); Found != Scope->end()) {
          const auto *Alias = Found->second;
          std::vector<std::string> Parameters;
          for (const auto &Part : Alias->children)
            if (Part->kind == K::ast_generic_parameter)
              Parameters.push_back(Part->text);
          if (Parameters.size() != Node->children.size() ||
              std::find(ExpandingAliases.begin(), ExpandingAliases.end(), Alias) !=
                  ExpandingAliases.end()) {
            Error(Node->Loc, "invalid recursive or mismatched generic alias");
            return;
          }
          auto Replacement = Clone(*Alias->children.back());
          std::unordered_map<std::string, const lex::Node *> Bindings;
          for (std::size_t I = 0; I < Parameters.size(); ++I)
            Bindings.emplace(Parameters[I], Node->children[I].get());
          Substitute(Replacement, Bindings);
          Replacement->Loc = Node->Loc;
          ExpandingAliases.push_back(Alias);
          Lower(Replacement, Module);
          ExpandingAliases.pop_back();
          Node = std::move(Replacement);
          return;
        }
    auto *Source = FindTemplate(Name, Module, &Node->Loc);
    if (!Apply && !Source && LowerAssociatedAlias(Node, Module, Name))
      return;
    std::string MemberSuffix;
    if (!Source && !Apply) {
      const auto Dot = Name.rfind('.');
      if (Dot != std::string::npos) {
        Source = FindTemplate(std::string_view(Name).substr(0, Dot), Module, &Node->Loc);
        if (Source && Source->Declaration->kind == K::ast_class)
          MemberSuffix = Name.substr(Dot);
        else
          Source = nullptr;
      }
    }
    if (!Source) {
      Error(Node->Loc, "unknown generic class, function, or alias");
      return;
    }
    if ((Apply && Source->Declaration->kind != K::ast_function &&
         Source->Declaration->kind != K::ast_class) ||
        (!Apply && Source->Declaration->kind != K::ast_class &&
         Source->Declaration->kind != K::ast_alias_decl)) {
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
      Replacement->Loc.End = Node->Loc.End;
      Node = std::move(Replacement);
    } else {
      Node->kind = K::ast_type;
      Node->text = Source->ModuleName.empty() ? Concrete + MemberSuffix
                                              : Source->ModuleName + "." + Concrete + MemberSuffix;
      Node->children.clear();
    }
  }

public:
  Implementation(const std::vector<const Module *> &Modules, ExpansionWorklist &Work)
      : Modules(Modules), Work(Work) {}

  bool Initialize() {
    for (const auto *Module : Modules) {
      const auto Name = Module->Name;
      auto &Declarations = Module->Lex.root->children;
      for (auto It = Declarations.begin(); It != Declarations.end();) {
        std::vector<std::string> Parameters;
        bool Pack = false;
        for (const auto &Child : (*It)->children)
          if (Child->kind == K::ast_generic_parameter || Child->kind == K::ast_generic_pack) {
            if (Pack)
              Error(Child->Loc, "generic type pack must be last");
            Pack = Child->kind == K::ast_generic_pack;
            Parameters.push_back(Child->text);
          }
        if (Parameters.empty()) {
          Work.emplace_back(It->get(), Module);
          ++It;
          continue;
        }
        std::unordered_set<std::string> Seen;
        for (const auto &Parameter : Parameters)
          if (!Seen.insert(Parameter).second)
            Error((*It)->Loc, "duplicate generic type parameter");
        const auto Key = Name.empty() ? (*It)->text : Name + "." + (*It)->text;
        const auto Loc = (*It)->Loc;
        if (Pack && (*It)->kind != K::ast_function)
          Error(Loc, "generic type packs are only supported on functions");
        Template Source{std::move(*It), Module, Name, std::move(Parameters), Pack};
        if (!Templates.emplace(Key, std::move(Source)).second)
          Error(Loc, "duplicate generic declaration");
        It = Declarations.erase(It);
      }
    }
    return Valid;
  }

  bool Expand(lex::Node &Node, const Module &Source) {
    const auto Module = Source.Name;
    if (Node.kind == K::ast_class) {
      AliasScopes.emplace_back();
      for (const auto &Child : Node.children)
        if (Child->kind == K::ast_alias_decl)
          RegisterScopedAlias(*Child);
    }
    for (auto &Child : Node.children)
      Lower(Child, Module);
    if (Node.kind == K::ast_class)
      AliasScopes.pop_back();
    return Valid;
  }

  bool IsTemplate(std::string_view Name, std::string_view Module) {
    return FindTemplate(Name, Module) != nullptr;
  }

  bool Finish() {
    for (auto &[Owner, Declarations] : PendingAliases)
      for (auto &Declaration : Declarations)
        Owner->children.push_back(std::move(Declaration));
    return Valid;
  }
};

GenericExpander::GenericExpander(const std::vector<const Module *> &Modules,
                                 ExpansionWorklist &Work)
    : Impl(std::make_unique<Implementation>(Modules, Work)) {}
GenericExpander::~GenericExpander() = default;
bool GenericExpander::Initialize() { return Impl->Initialize(); }
bool GenericExpander::Expand(lex::Node &Node, const Module &Source) {
  return Impl->Expand(Node, Source);
}
bool GenericExpander::IsTemplate(std::string_view Name, std::string_view Module) {
  return Impl->IsTemplate(Name, Module);
}
bool GenericExpander::Finish() { return Impl->Finish(); }
