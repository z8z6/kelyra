#include "Driver/Generics.h"
#include "BuiltinAnnotations.h"
#include "BuiltinMeta.h"

#include "Sema/Type.h"
#include "Sema/ConstEval.h"
#include "Support/BuiltinAnnotation.h"
#include "Support/Log.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
  Result->BoundFieldName = Source.BoundFieldName;
  Result->AssociatedOwnerArguments = Source.AssociatedOwnerArguments;
  Result->GenericOriginModule = Source.GenericOriginModule;
  Result->AnnotationOriginModule = Source.AnnotationOriginModule;
  for (const auto &Child : Source.children)
    Result->children.push_back(Clone(*Child));
  return Result;
}

void SetGeneratedLocation(lex::Node &Node, const lex::Location &Loc) {
  Node.Loc = Loc;
  for (auto &Child : Node.children)
    SetGeneratedLocation(*Child, Loc);
}

std::string TypeName(const lex::Node &Node) {
  if (Node.kind == K::ast_type)
    return Node.text;
  if (Node.kind == K::ast_pointer_type && Node.children.size() == 1)
    return "*" + TypeName(*Node.children.front());
  if (Node.kind == K::ast_array_type && Node.children.size() == 2) {
    sema::ConstEvaluator Evaluator;
    const auto Length = Evaluator.Evaluate(*Node.children.front());
    return "[" +
           (Length && Length->Type == sema::ConstValue::Kind::Integer
                ? Length->Text
                : Node.text) +
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
  bool Pack = false;
};

struct AnnotationDefinition {
  const lex::Node *Declaration = nullptr;
  std::string Module;
  std::string TargetBinding;
  bool FieldTarget = false;
  const lex::Node *Composition = nullptr;
  const lex::Node *Body = nullptr;
};

struct ClassSnapshot {
  std::string QualifiedName;
  bool Public = false;
  std::unordered_set<std::string> Fields;
  std::unordered_set<std::string> Functions;
};

AnnotationDefinition ReadAnnotationDefinition(const lex::Node &Declaration,
                                              std::string Module) {
  AnnotationDefinition Definition;
  Definition.Declaration = &Declaration;
  Definition.Module = std::move(Module);
  for (const auto &Child : Declaration.children) {
    if (Child->kind == K::ast_annotation &&
        (Child->text == "target" || Child->text == "std.annotation.target") &&
        Child->children.size() == 1 &&
        Child->children.front()->children.size() == 1) {
      const auto &Kind = *Child->children.front()->children.front();
      const bool ClassTarget = Kind.text == "Target.Class" ||
                               Kind.text == "std.annotation.Target.Class";
      const bool FieldTarget = Kind.text == "Target.Field" ||
                               Kind.text == "std.annotation.Target.Field";
      if ((ClassTarget || FieldTarget) && Kind.children.size() == 1) {
        Definition.TargetBinding = Kind.children.front()->text;
        Definition.FieldTarget = FieldTarget;
      }
    }
    if (Child->kind == K::ast_annotation_uses)
      Definition.Composition = Child.get();
    if (Child->kind == K::ast_annotation_body)
      Definition.Body = Child.get();
  }
  return Definition;
}

class GenericExpander {
  std::deque<SourceModule> &Modules;
  std::map<std::string, Template> Templates;
  std::vector<std::unordered_map<std::string, const lex::Node *>> AliasScopes;
  std::vector<const lex::Node *> ExpandingAliases;
  std::unordered_map<std::string, AnnotationDefinition> Annotations;
  std::optional<lex::ParseResult> BuiltinAnnotations;
  std::optional<lex::ParseResult> BuiltinMeta;
  const lex::Node *MetaModule = nullptr;
  std::unordered_map<const lex::Node *, ClassSnapshot> ClassSnapshots;
  std::unordered_map<std::string, std::string> Instances;
  std::deque<std::pair<lex::Node *, std::string>> Work;
  std::unordered_map<lex::Node *, std::vector<std::unique_ptr<lex::Node>>>
      PendingMethods;
  std::unordered_map<lex::Node *, std::vector<std::unique_ptr<lex::Node>>>
      PendingAliases;
  std::size_t NextAspectId = 0;
  bool Valid = true;

  void Error(const lex::Location &Loc, std::string_view Message) {
    kerr() << Loc.File << ':' << Loc.Line << ':' << Loc.Column
           << ": error: " << Message << '\n';
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
      if (Part->kind == K::ast_generic_parameter &&
          !Parameters.insert(Part->text).second)
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
    for (const auto &Input : Modules) {
      if (ModuleName(*Input.Parsed.root) != Module)
        continue;
      for (const auto &Child : Input.Parsed.root->children) {
        if (Child->kind != K::ast_import)
          continue;
        const auto Candidate =
            Templates.find(Child->text + "." + std::string(Name));
        if (Candidate == Templates.end() ||
            std::none_of(
                Candidate->second.Declaration->children.begin(),
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

  static void Substitute(
      std::unique_ptr<lex::Node> &Node,
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

  bool ExpandLocalAliases(std::unique_ptr<lex::Node> &Node,
                          std::unordered_set<const lex::Node *> &Visiting) {
    if (Node->kind == K::ast_type && Node->text.find('.') == std::string::npos)
      for (auto Scope = AliasScopes.rbegin(); Scope != AliasScopes.rend();
           ++Scope)
        if (const auto Found = Scope->find(Node->text); Found != Scope->end()) {
          const auto *Alias = Found->second;
          if (std::any_of(Alias->children.begin(), Alias->children.end(),
                          [](const auto &Part) {
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

  const AnnotationDefinition *FindAnnotation(std::string_view Name,
                                             std::string_view Module,
                                             const lex::Location &Loc) {
    const auto Find =
        [&](std::string_view Key) -> const AnnotationDefinition * {
      const auto It = Annotations.find(std::string(Key));
      return It == Annotations.end() ? nullptr : &It->second;
    };
    if (Name.find('.') != std::string_view::npos)
      return Find(Name);
    if (auto *Local = Find(Module.empty()
                               ? std::string(Name)
                               : std::string(Module) + "." + std::string(Name)))
      return Local;
    const AnnotationDefinition *Result =
        Find("std.annotation." + std::string(Name));
    for (const auto &Input : Modules) {
      if (ModuleName(*Input.Parsed.root) != Module)
        continue;
      for (const auto &Child : Input.Parsed.root->children) {
        if (Child->kind != K::ast_import)
          continue;
        auto *Candidate = Find(Child->text + "." + std::string(Name));
        if (!Candidate)
          continue;
        if (Result && Result != Candidate) {
          Error(Loc, "ambiguous annotation name");
          return nullptr;
        }
        Result = Candidate;
      }
    }
    return Result;
  }

  static void SubstituteValue(
      std::unique_ptr<lex::Node> &Node,
      const std::unordered_map<std::string, const lex::Node *> &Values) {
    if (Node->kind == K::ast_name) {
      if (const auto Found = Values.find(Node->text); Found != Values.end()) {
        Node = Clone(*Found->second);
        return;
      }
    }
    for (auto &Child : Node->children)
      SubstituteValue(Child, Values);
  }

  static void SubstituteTargetName(std::unique_ptr<lex::Node> &Node,
                                   std::string_view Binding,
                                   std::string_view Target) {
    if (Node->kind == K::ast_name && Node->text == Binding)
      Node->text = Target;
    for (auto &Child : Node->children)
      SubstituteTargetName(Child, Binding, Target);
  }

  static void SetAnnotationOrigin(lex::Node &Node, std::string_view Module,
                                  std::string_view TargetType) {
    Node.AnnotationOriginModule = Module;
    if (Node.kind == K::ast_type && Node.text != TargetType &&
        !Node.GenericArgument) {
      Node.GenericArgument = true;
      Node.GenericOriginModule = Module;
    }
    for (auto &Child : Node.children)
      SetAnnotationOrigin(*Child, Module, TargetType);
  }

  static void SubstituteField(std::unique_ptr<lex::Node> &Node,
                              std::string_view Binding, const lex::Node &Field,
                              const lex::Node &FieldType,
                              std::string_view TargetModule) {
    if (Node->kind == K::ast_type &&
        Node->text == std::string(Binding) + ".type") {
      Node = Clone(FieldType);
      BindArgument(*Node, TargetModule);
      return;
    }
    const bool Direct =
        (Node->kind == K::ast_name || Node->kind == K::ast_member) &&
        Node->text == Binding;
    if (Direct) {
      Node->text = Field.text;
      if (Node->kind == K::ast_name)
        Node->BoundFieldName = true;
    }
    for (auto &Child : Node->children)
      SubstituteField(Child, Binding, Field, FieldType, TargetModule);
  }

  static std::optional<std::string> EvaluateNamePart(
      const lex::Node &Expression,
      const std::unordered_map<std::string, const lex::Node *> &Values,
      std::string_view Binding, std::string_view Target, unsigned Depth = 0) {
    if (Depth > 32)
      return std::nullopt;
    sema::ConstEvaluator Evaluator([&](const lex::Node &Node)
                                       -> std::optional<sema::ConstValue> {
      if (Node.kind == K::ast_name) {
        if (const auto Found = Values.find(Node.text); Found != Values.end()) {
          auto Value = EvaluateNamePart(*Found->second, Values, Binding,
                                        Target, Depth + 1);
          return Value ? std::optional<sema::ConstValue>(
                             sema::ConstValue::String(*Value))
                       : std::nullopt;
        }
      }
      if (Node.kind == K::ast_member && Node.text == "name" &&
          Node.children.size() == 1 &&
          Node.children.front()->kind == K::ast_name &&
          Node.children.front()->text == Binding)
        return sema::ConstValue::String(std::string(Target));
      return std::nullopt;
    });
    auto Result = Evaluator.Evaluate(Expression);
    return Result && Result->Type == sema::ConstValue::Kind::String
               ? std::optional<std::string>(Result->Text)
               : std::nullopt;
  }

  bool InterpolateNames(
      lex::Node &Node,
      const std::unordered_map<std::string, const lex::Node *> &Values,
      std::string_view Binding, std::string_view Target) {
    if (Node.kind != K::ast_literal &&
        Node.text.find("${") != std::string::npos) {
      if (Node.kind != K::ast_name && Node.kind != K::ast_member &&
          Node.kind != K::ast_function && Node.kind != K::ast_parameter &&
          Node.kind != K::ast_field && Node.kind != K::ast_const_field &&
          Node.kind != K::ast_type && Node.kind != K::ast_generic_type &&
          Node.kind != K::ast_annotation) {
        Error(Node.Loc, "identifier interpolation is not allowed here");
        return false;
      }
      std::string Result;
      std::size_t Position = 0;
      while (true) {
        const auto Open = Node.text.find("${", Position);
        if (Open == std::string::npos)
          break;
        Result += Node.text.substr(Position, Open - Position);
        unsigned Braces = 1;
        std::size_t Close = Open + 2;
        bool Quoted = false;
        for (; Close < Node.text.size() && Braces != 0; ++Close) {
          const char Part = Node.text[Close];
          if (Quoted && Part == '\\' && Close + 1 < Node.text.size()) {
            ++Close;
          } else if (Part == '"') {
            Quoted = !Quoted;
          } else if (!Quoted && Part == '{') {
            ++Braces;
          } else if (!Quoted && Part == '}') {
            --Braces;
          }
        }
        if (Braces != 0) {
          Error(Node.Loc, "unterminated identifier interpolation");
          return false;
        }
        auto Parsed = lex::Lexer().parseExpression(
            Node.text.substr(Open + 2, Close - Open - 3));
        if (!Parsed.ok() || !Parsed.root) {
          Error(Node.Loc, "invalid identifier interpolation expression");
          return false;
        }
        auto Part = EvaluateNamePart(*Parsed.root, Values, Binding, Target);
        if (!Part) {
          Error(Node.Loc,
                "identifier interpolation requires a compile-time string");
          return false;
        }
        Result += *Part;
        Position = Close;
      }
      Result += Node.text.substr(Position);
      auto Parsed = lex::Lexer().parseExpression(Result);
      if (!Parsed.ok() || !Parsed.root || Parsed.root->kind != K::ast_name ||
          Parsed.root->text != Result) {
        Error(Node.Loc, "identifier interpolation produced an invalid name");
        return false;
      }
      Node.text = std::move(Result);
    }
    for (auto &Child : Node.children)
      if (!InterpolateNames(*Child, Values, Binding, Target))
        return false;
    return true;
  }

  std::string MetaOperation(std::string_view Callee) const {
    if (!MetaModule)
      return {};
    for (const auto &Function : MetaModule->children) {
      if (Function->kind != K::ast_function ||
          Callee != "std.meta." + Function->text)
        continue;
      for (const auto &Part : Function->children) {
        if (Part->kind != K::ast_annotation ||
            !IsBuiltinAnnotation(Part->text, "intrinsic"))
          continue;
        if (Part->children.empty())
          return Function->text;
        if (Part->children.size() != 1 ||
            Part->children.front()->children.size() != 1)
          return {};
        const auto &Value = Part->children.front()->children.front()->text;
        if (Value.size() >= 2 && Value.front() == '"' && Value.back() == '"')
          return Value.substr(1, Value.size() - 2);
      }
    }
    return {};
  }

  std::optional<bool>
  EvaluateAnnotationCondition(const lex::Node &Expression,
                              const ClassSnapshot *Snapshot) const {
    sema::ConstEvaluator Evaluator(
        [&](const lex::Node &Node) -> std::optional<sema::ConstValue> {
          if (!Snapshot || Node.kind != K::ast_meta ||
              Node.children.size() != 1 ||
              Node.children.front()->kind != K::ast_type ||
              Node.children.front()->text != Snapshot->QualifiedName)
            return std::nullopt;
          return sema::ConstValue{sema::ConstValue::Kind::Symbol, "0"};
        },
        [&](const lex::Node &Callee) -> const lex::Node * {
          if (!Snapshot || !MetaModule || Callee.kind != K::ast_member ||
              Callee.children.size() != 1 ||
              Callee.children.front()->kind != K::ast_meta ||
              Callee.children.front()->children.size() != 1 ||
              Callee.children.front()->children.front()->kind != K::ast_type ||
              Callee.children.front()->children.front()->text !=
                  Snapshot->QualifiedName)
            return nullptr;
          for (const auto Name : {"Class", "Symbol"})
            for (const auto &Class : MetaModule->children)
              if (Class->kind == K::ast_class && Class->text == Name)
                for (const auto &Method : Class->children)
                  if (Method->kind == K::ast_function &&
                      Method->text == Callee.text)
                    return Method.get();
          return nullptr;
        },
        [&](const lex::Node &Call, const std::vector<sema::ConstValue> &Args)
            -> std::optional<sema::ConstValue> {
          if (!Snapshot || Call.kind != K::ast_call || Call.children.empty() ||
              Args.empty() ||
              Args.front().Type != sema::ConstValue::Kind::Integer ||
              Args.front().Text != "0")
            return std::nullopt;
          const auto Operation =
              MetaOperation(CalleeName(*Call.children.front()));
          if (Args.size() == 1 &&
              (Operation == "__read_public" || Operation == "meta.read_public"))
            return sema::ConstValue::Bool(Snapshot->Public);
          if (Args.size() != 2 ||
              Args[1].Type != sema::ConstValue::Kind::String)
            return std::nullopt;
          const auto &Name = Args[1].Text;
          if (Operation == "__has_field")
            return sema::ConstValue::Bool(Snapshot->Fields.contains(Name));
          if (Operation == "__has_function")
            return sema::ConstValue::Bool(Snapshot->Functions.contains(Name));
          if (Operation == "__has_member")
            return sema::ConstValue::Bool(Snapshot->Fields.contains(Name) ||
                                          Snapshot->Functions.contains(Name));
          return std::nullopt;
        },
        [&](const sema::ConstValue &Owner,
            std::string_view Name) -> std::optional<sema::ConstValue> {
          if (!Snapshot || Owner.Type != sema::ConstValue::Kind::Symbol ||
              Owner.Text != "0" || Name != "name")
            return std::nullopt;
          const auto Dot = Snapshot->QualifiedName.rfind('.');
          return sema::ConstValue::String(Snapshot->QualifiedName.substr(
              Dot == std::string::npos ? 0 : Dot + 1));
        });
    auto Result = Evaluator.Evaluate(Expression);
    return Result && Result->Type == sema::ConstValue::Kind::Bool
               ? std::optional<bool>(Result->Text == "true")
               : std::nullopt;
  }

  bool ExpandUse(lex::Node &Target, const lex::Node &Use,
                 std::string_view Module, std::vector<std::string> &Stack,
                 lex::Node *Owner, std::string_view TargetModule) {
    const auto *Definition = FindAnnotation(Use.text, Module, Use.Loc);
    if (!Definition || (!Definition->Composition && !Definition->Body))
      return Valid;
    const auto Key =
        Definition->Module.empty()
            ? Definition->Declaration->text
            : Definition->Module + "." + Definition->Declaration->text;
    if (std::find(Stack.begin(), Stack.end(), Key) != Stack.end()) {
      Error(Use.Loc, "cyclic annotation composition");
      return false;
    }
    if (Stack.size() >= 32) {
      Error(Use.Loc, "annotation expansion limit exceeded");
      return false;
    }
    Stack.push_back(Key);
    std::vector<const lex::Node *> Parameters;
    for (const auto &Child : Definition->Declaration->children)
      if (Child->kind == K::ast_annotation_parameter)
        Parameters.push_back(Child.get());
    std::unordered_map<std::string, const lex::Node *> Values;
    std::size_t Position = 0;
    for (const auto &Argument : Use.children) {
      if (Argument->kind != K::ast_annotation_argument ||
          Argument->children.size() != 1)
        continue;
      const lex::Node *Parameter = nullptr;
      if (Argument->text.empty()) {
        if (Position < Parameters.size())
          Parameter = Parameters[Position++];
      } else {
        for (const auto *Candidate : Parameters)
          if (Candidate->text == Argument->text)
            Parameter = Candidate;
      }
      if (Parameter)
        Values.emplace(Parameter->text, Argument->children.front().get());
    }
    for (const auto *Parameter : Parameters)
      if (!Values.contains(Parameter->text) && Parameter->children.size() == 2)
        Values.emplace(Parameter->text, Parameter->children.back().get());
    if (Definition->Composition)
      for (const auto &Part : Definition->Composition->children) {
        auto Expanded = Clone(*Part);
        for (auto &Argument : Expanded->children)
          SubstituteValue(Argument, Values);
        const auto *Component =
            FindAnnotation(Expanded->text, Definition->Module, Expanded->Loc);
        if (!Component) {
          Error(Expanded->Loc, "unknown composed annotation");
          return false;
        }
        Expanded->text =
            Component->Module.empty()
                ? Component->Declaration->text
                : Component->Module + "." + Component->Declaration->text;
        Expanded->AnnotationOriginModule = Definition->Module;
        auto *Generated = Expanded.get();
        Target.children.push_back(std::move(Expanded));
        if (!ExpandUse(Target, *Generated, Definition->Module, Stack, Owner,
                       TargetModule))
          return false;
      }
    if (Definition->Body) {
      const bool FieldTarget = Definition->FieldTarget;
      if (Definition->TargetBinding.empty() ||
          (FieldTarget ? (!Owner || Owner->kind != K::ast_class ||
                          (Target.kind != K::ast_field &&
                           Target.kind != K::ast_const_field))
                       : Target.kind != K::ast_class)) {
        Error(Use.Loc, "annotation body requires a bound target");
        return false;
      }
      const lex::Node *FieldType = nullptr;
      if (FieldTarget)
        for (const auto &Part : Target.children)
          if (Part->kind == K::ast_type || Part->kind == K::ast_pointer_type ||
              Part->kind == K::ast_array_type ||
              Part->kind == K::ast_slice_type ||
              Part->kind == K::ast_generic_type ||
              Part->kind == K::ast_function_type)
            FieldType = Part.get();
      lex::Node Bound;
      std::unordered_map<std::string, const lex::Node *> Binding;
      if (!FieldTarget) {
        Bound.kind = K::ast_type;
        Bound.Loc = Target.Loc;
        Bound.text = TargetModule.empty()
                         ? Target.text
                         : std::string(TargetModule) + "." + Target.text;
        Binding.emplace(Definition->TargetBinding, &Bound);
      } else if (!FieldType) {
        Error(Use.Loc, "field annotation target has no type");
        return false;
      }
      std::vector<const lex::Node *> Members;
      std::function<bool(const lex::Node &, unsigned)> Select =
          [&](const lex::Node &Part, unsigned Depth) {
            if (Depth > 32) {
              Error(Part.Loc, "annotation condition nesting limit exceeded");
              return false;
            }
            if (Part.kind != K::ast_when) {
              Members.push_back(&Part);
              return true;
            }
            if (Part.children.size() < 2 || Part.children.size() > 3) {
              Error(Part.Loc, "invalid annotation condition");
              return false;
            }
            auto Condition = Clone(*Part.children.front());
            SubstituteValue(Condition, Values);
            if (!FieldTarget)
              Substitute(Condition, Binding);
            const auto Found =
                ClassSnapshots.find(FieldTarget ? Owner : &Target);
            const ClassSnapshot *Snapshot =
                Found == ClassSnapshots.end() ? nullptr : &Found->second;
            const auto Result =
                EvaluateAnnotationCondition(*Condition, Snapshot);
            if (!Result) {
              Error(Part.children.front()->Loc,
                    "annotation when requires a compile-time boolean");
              return false;
            }
            const lex::Node *Branch = *Result ? Part.children[1].get()
                                      : Part.children.size() == 3
                                          ? Part.children[2].get()
                                          : nullptr;
            if (!Branch)
              return true;
            if (Branch->kind == K::ast_when)
              return Select(*Branch, Depth + 1);
            for (const auto &Member : Branch->children)
              if (!Select(*Member, Depth + 1))
                return false;
            return true;
          };
      for (const auto &Part : Definition->Body->children)
        if (!Select(*Part, 0))
          return false;
      for (const auto *Part : Members) {
        if ((!FieldTarget && Part->kind != K::ast_field &&
             Part->kind != K::ast_function) ||
            (FieldTarget && Part->kind != K::ast_function)) {
          Error(Part->Loc,
                "annotation body may only inject fields and methods");
          return false;
        }
        auto Member = Clone(*Part);
        if (!InterpolateNames(*Member, Values, Definition->TargetBinding,
                              Target.text))
          return false;
        SubstituteValue(Member, Values);
        if (!FieldTarget) {
          Substitute(Member, Binding);
          SubstituteTargetName(Member, Definition->TargetBinding, Bound.text);
          SetAnnotationOrigin(*Member, Definition->Module, Bound.text);
          Target.children.push_back(std::move(Member));
          continue;
        }
        SetAnnotationOrigin(*Member, Definition->Module, {});
        SubstituteField(Member, Definition->TargetBinding, Target, *FieldType,
                        TargetModule);
        SetGeneratedLocation(*Member, Use.Loc);
        PendingMethods[Owner].push_back(std::move(Member));
      }
    }
    Stack.pop_back();
    return Valid;
  }

  bool ExpandAspects(lex::Node &Function, lex::Node *Owner,
                     SourceModule &Source, std::string_view Module) {
    std::vector<std::string> Handlers;
    for (auto It = Function.children.begin(); It != Function.children.end();) {
      if ((*It)->kind != K::ast_annotation ||
          !IsBuiltinAnnotation((*It)->text, "aspect")) {
        ++It;
        continue;
      }
      const auto &Use = **It;
      if (Use.children.size() != 1 ||
          Use.children.front()->children.size() != 1) {
        Error(Use.Loc, "@aspect expects one function name");
        return false;
      }
      const auto &Handler = *Use.children.front()->children.front();
      if (Handler.kind != K::ast_meta || Handler.children.size() != 1) {
        Error(Use.Loc, "@aspect expects meta(function)");
        return false;
      }
      const auto &Target = *Handler.children.front();
      const auto Name =
          Target.kind == K::ast_type ? Target.text : CalleeName(Target);
      if (Name.empty()) {
        Error(Use.Loc, "@aspect expects a function name");
        return false;
      }
      Handlers.push_back(Name);
      It = Function.children.erase(It);
    }
    if (Handlers.empty())
      return true;
    const auto Block =
        std::find_if(Function.children.begin(), Function.children.end(),
                     [](const auto &P) { return P->kind == K::ast_block; });
    if (Block == Function.children.end()) {
      Error(Function.Loc, "@aspect requires a function body");
      return false;
    }
    const bool Method = Owner && Owner->kind == K::ast_class;
    const bool Static = std::any_of(
        Function.children.begin(), Function.children.end(), [](const auto &P) {
          return P->kind == K::ast_annotation &&
                 IsBuiltinAnnotation(P->text, "static");
        });
    const bool Receiver = Method && !Static;
    std::vector<const lex::Node *> Parameters;
    const lex::Node *Return = nullptr;
    for (const auto &Part : Function.children) {
      if (Part->kind == K::ast_parameter)
        Parameters.push_back(Part.get());
      if (Part->kind == K::ast_type || Part->kind == K::ast_pointer_type ||
          Part->kind == K::ast_array_type || Part->kind == K::ast_slice_type ||
          Part->kind == K::ast_generic_type ||
          Part->kind == K::ast_function_type)
        Return = Part.get();
    }
    if (!Return || TypeName(*Return).empty()) {
      Error(Function.Loc, "@aspect requires a supported return type");
      return false;
    }
    const std::string ResultType = TypeName(*Return);
    const bool Void = ResultType == "void";
    std::vector<std::string> ParameterTypes;
    for (const auto *Parameter : Parameters) {
      if (Parameter->children.empty()) {
        Error(Parameter->Loc, "@aspect requires typed parameters");
        return false;
      }
      const auto Type = TypeName(*Parameter->children.back());
      if (Type.empty()) {
        Error(Parameter->Loc, "@aspect requires supported parameter types");
        return false;
      }
      ParameterTypes.push_back(Type);
    }
    for (auto Handler = Handlers.rbegin(); Handler != Handlers.rend();
         ++Handler) {
      const auto Id = std::to_string(NextAspectId++);
      const std::string OriginalName = "__aspect_original_" + Id;
      const std::string ThunkName = "__aspect_thunk_" + Id;
      auto Original = Clone(Function);
      Original->text = OriginalName;
      Original->children.erase(
          std::remove_if(
              Original->children.begin(), Original->children.end(),
              [](const auto &Part) {
                return Part->kind == K::ast_public ||
                       (Part->kind == K::ast_annotation &&
                        (IsBuiltinAnnotation(Part->text, "main") ||
                         IsBuiltinAnnotation(Part->text, "virtual") ||
                         IsBuiltinAnnotation(Part->text, "override")));
              }),
          Original->children.end());
      if (Method)
        PendingMethods[Owner].push_back(std::move(Original));
      else {
        auto *Generated = Original.get();
        Source.Parsed.root->children.push_back(std::move(Original));
        Work.emplace_back(Generated, std::string(Module));
      }

      std::string Call =
          Method ? (Receiver ? "__aspect_receiver." : Owner->text + ".") : "";
      Call += OriginalName + "(";
      std::string Thunk =
          "fn " + ThunkName + "(context: *u8) -> " + ResultType + " {\n";
      const auto Count = Parameters.size() + (Receiver ? 1 : 0);
      Thunk += "let __aspect_slots = context as *[" +
               std::to_string(std::max<std::size_t>(Count, 1)) + "]usize;\n";
      if (Receiver)
        Thunk += "let __aspect_receiver = (*__aspect_slots)[0] as *" +
                 Owner->text + ";\n";
      for (std::size_t I = 0; I < Parameters.size(); ++I) {
        if (I)
          Call += ", ";
        Call += "*(((*__aspect_slots)[" +
                std::to_string(I + (Receiver ? 1 : 0)) + "]) as *" +
                ParameterTypes[I] + ")";
      }
      Call += ")";
      Thunk += Void ? Call + ";\nreturn;\n}\n" : "return " + Call + ";\n}\n";
      auto ParsedThunk = lex::Lexer().parse(Thunk, Function.Loc.File);
      if (!ParsedThunk.ok() || ParsedThunk.root->children.empty()) {
        Error(Function.Loc, "cannot generate aspect continuation");
        return false;
      }
      auto Continuation = std::move(ParsedThunk.root->children.front());
      SetGeneratedLocation(*Continuation, Function.Loc);
      auto *ContinuationNode = Continuation.get();
      Source.Parsed.root->children.push_back(std::move(Continuation));
      Work.emplace_back(ContinuationNode, std::string(Module));

      std::string Body = "fn __wrapper() -> void {\n";
      Body += "let __aspect_context: [" +
              std::to_string(std::max<std::size_t>(Count, 1)) + "]usize;\n";
      if (Receiver)
        Body += "__aspect_context[0] = this as usize;\n";
      for (std::size_t I = 0; I < Parameters.size(); ++I)
        Body += "__aspect_context[" + std::to_string(I + (Receiver ? 1 : 0)) +
                "] = (&" + Parameters[I]->text + ") as usize;\n";
      Body += "let __aspect_invocation = std.aspect.";
      Body += Void ? "VoidInvocation" : "Invocation<" + ResultType + ">";
      Body += "((&__aspect_context[0]) as *u8, " + ThunkName + ");\n";
      std::string AspectCall = *Handler;
      if (!Void && FindTemplate(*Handler, Module))
        AspectCall += "<" + ResultType + ">";
      AspectCall += "(&__aspect_invocation)";
      Body += Void ? AspectCall + ";\nreturn;\n}\n"
                   : "return " + AspectCall + ";\n}\n";
      auto ParsedBody = lex::Lexer().parse(Body, Function.Loc.File);
      if (!ParsedBody.ok() || ParsedBody.root->children.empty()) {
        Error(Function.Loc, "cannot generate aspect wrapper");
        return false;
      }
      for (auto &Part : ParsedBody.root->children.front()->children)
        if (Part->kind == K::ast_block) {
          SetGeneratedLocation(*Part, Function.Loc);
          *Block = std::move(Part);
          break;
        }
    }
    return true;
  }

  bool ExpandAnnotations(lex::Node &Node, std::string_view Module,
                         SourceModule &Source, lex::Node *Owner = nullptr) {
    if (Node.kind == K::ast_class) {
      ClassSnapshot Snapshot;
      Snapshot.QualifiedName =
          Module.empty() ? Node.text : std::string(Module) + "." + Node.text;
      for (const auto &Child : Node.children) {
        if (Child->kind == K::ast_public)
          Snapshot.Public = true;
        else if (Child->kind == K::ast_field ||
                 Child->kind == K::ast_const_field)
          Snapshot.Fields.insert(Child->text);
        else if (Child->kind == K::ast_function ||
                 Child->kind == K::ast_constructor ||
                 Child->kind == K::ast_destructor)
          Snapshot.Functions.insert(Child->text);
      }
      ClassSnapshots.emplace(&Node, std::move(Snapshot));
    }
    const bool Field =
        Node.kind == K::ast_field || Node.kind == K::ast_const_field;
    const auto Before = Field && Owner ? PendingMethods[Owner].size() : 0;
    std::vector<const lex::Node *> Uses;
    for (const auto &Child : Node.children)
      if (Child->kind == K::ast_annotation)
        Uses.push_back(Child.get());
    for (const auto *Use : Uses) {
      std::vector<std::string> Stack;
      if (!ExpandUse(Node, *Use, Module, Stack, Owner, Module))
        return false;
    }
    if (Field && Owner) {
      const lex::Node *Static = nullptr;
      for (const auto &Part : Node.children)
        if (Part->kind == K::ast_annotation &&
            IsBuiltinAnnotation(Part->text, "static"))
          Static = Part.get();
      if (Static)
        for (std::size_t I = Before; I < PendingMethods[Owner].size(); ++I)
          PendingMethods[Owner][I]->children.insert(
              PendingMethods[Owner][I]->children.begin(), Clone(*Static));
    }
    if (Node.kind == K::ast_function &&
        !ExpandAspects(Node, Owner, Source, Module))
      return false;
    for (auto &Child : Node.children)
      if (Child->kind == K::ast_field || Child->kind == K::ast_const_field ||
          Child->kind == K::ast_function || Child->kind == K::ast_constructor ||
          Child->kind == K::ast_destructor)
        if (!ExpandAnnotations(*Child, Module, Source, &Node))
          return false;
    if (Node.kind == K::ast_class) {
      if (auto Found = PendingMethods.find(&Node);
          Found != PendingMethods.end()) {
        for (auto &Method : Found->second)
          Node.children.push_back(std::move(Method));
        PendingMethods.erase(Found);
      }
    }
    return true;
  }

  static bool ExpandPackUses(std::unique_ptr<lex::Node> &Node,
                             std::string_view PackName, std::size_t Count) {
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
          if (Argument->children.size() != 1 ||
              Argument->children.front()->kind != K::ast_name ||
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

  std::string Instantiate(const Template &Source,
                          const std::vector<const lex::Node *> &Arguments,
                          const lex::Location &Loc,
                          std::string_view CallerModule) {
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
    for (std::size_t I = 0; I < Source.Parameters.size() - Source.Pack; ++I)
      Bindings.emplace(Source.Parameters[I], BoundArguments[I].get());
    std::erase_if(Declaration->children, [](const auto &Child) {
      return Child->kind == K::ast_generic_parameter ||
             Child->kind == K::ast_generic_pack;
    });
    for (auto &Child : Declaration->children)
      Substitute(Child, Bindings);
    if (Source.Pack) {
      const auto PackTypeName = Source.Parameters.back();
      const auto Count = Arguments.size() - (Source.Parameters.size() - 1);
      auto Parameter =
          std::find_if(Declaration->children.begin(),
                       Declaration->children.end(), [](const auto &Child) {
                         return Child->kind == K::ast_parameter_pack;
                       });
      if (Parameter == Declaration->children.end() ||
          (*Parameter)->children.empty() ||
          (*Parameter)->children.back()->kind != K::ast_type ||
          (*Parameter)->children.back()->text != PackTypeName ||
          std::any_of(std::next(Parameter), Declaration->children.end(),
                      [](const auto &Child) {
                        return Child->kind == K::ast_parameter ||
                               Child->kind == K::ast_parameter_pack;
                      })) {
        Error(Loc, "generic parameter pack requires a trailing parameter pack");
        return {};
      }
      for (const auto &Child : (*Parameter)->children)
        if (Child->kind == K::ast_annotation &&
            (Child->text == "forward" ||
             Child->text == "std.annotation.forward")) {
          Error(Child->Loc,
                "@forward on function parameter packs is not supported");
          return {};
        }
      const auto Position =
          std::distance(Declaration->children.begin(), Parameter);
      const auto PackVariableName = (*Parameter)->text;
      Declaration->children.erase(Parameter);
      for (std::size_t I = 0; I < Count; ++I) {
        auto Concrete = std::make_unique<lex::Node>();
        Concrete->kind = K::ast_parameter;
        Concrete->Loc = Loc;
        Concrete->text = "$pack" + std::to_string(I);
        Concrete->children.push_back(
            Clone(*BoundArguments[Source.Parameters.size() - 1 + I]));
        Declaration->children.insert(
            Declaration->children.begin() + Position + I, std::move(Concrete));
      }
      for (auto &Child : Declaration->children)
        if (Child->kind == K::ast_block &&
            !ExpandPackUses(Child, PackVariableName, Count)) {
          Error(Child->Loc, "invalid parameter pack use");
          return {};
        }
    }
    auto *Generated = Declaration.get();
    Source.Module->Parsed.root->children.push_back(std::move(Declaration));
    Work.emplace_back(Generated, Source.ModuleName);
    return Name;
  }

  bool LowerAssociatedAlias(std::unique_ptr<lex::Node> &Node,
                            std::string_view Module, std::string_view Name) {
    const auto Dot = Name.rfind('.');
    if (Dot == std::string_view::npos)
      return false;
    const auto OwnerName = Name.substr(0, Dot);
    const auto AliasName = Name.substr(Dot + 1);
    lex::Node *Owner = nullptr;
    std::string OwnerModule;
    if (const auto *Generic = FindTemplate(OwnerName, Module, &Node->Loc);
        Node->AssociatedOwnerArguments && Generic &&
        Generic->Declaration->kind == K::ast_class) {
      std::vector<const lex::Node *> OwnerArguments;
      for (std::size_t I = 0; I < Node->AssociatedOwnerArguments; ++I)
        OwnerArguments.push_back(Node->children[I].get());
      const auto Concrete =
          Instantiate(*Generic, OwnerArguments, Node->Loc, Module);
      if (Concrete.empty())
        return true;
      OwnerModule = Generic->ModuleName;
      for (const auto &Part : Generic->Module->Parsed.root->children)
        if (Part->kind == K::ast_class && Part->text == Concrete) {
          Owner = Part.get();
          break;
        }
    } else if (!Node->AssociatedOwnerArguments) {
      auto FindClass = [&](std::string_view CandidateModule,
                           std::string_view CandidateName) -> lex::Node * {
        for (auto &Input : Modules)
          if (ModuleName(*Input.Parsed.root) == CandidateModule)
            for (auto &Part : Input.Parsed.root->children)
              if (Part->kind == K::ast_class && Part->text == CandidateName)
                return Part.get();
        return nullptr;
      };
      if (const auto Split = OwnerName.rfind('.');
          Split != std::string_view::npos) {
        OwnerModule = std::string(OwnerName.substr(0, Split));
        Owner = FindClass(OwnerModule, OwnerName.substr(Split + 1));
      } else {
        OwnerModule = std::string(Module);
        Owner = FindClass(OwnerModule, OwnerName);
        if (!Owner)
          for (const auto &Input : Modules)
            if (ModuleName(*Input.Parsed.root) == Module)
              for (const auto &Import : Input.Parsed.root->children)
                if (Import->kind == K::ast_import) {
                  auto *Candidate = FindClass(Import->text, OwnerName);
                  if (!Candidate || std::none_of(Candidate->children.begin(),
                                                 Candidate->children.end(),
                                                 [](const auto &Part) {
                                                   return Part->kind ==
                                                          K::ast_public;
                                                 }))
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
      return std::any_of(
          Declaration.children.begin(), Declaration.children.end(),
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
    const auto Key = "member:" + OwnerModule + "." + Owner->text + "." +
                     Alias->text + "<" + Signature + ">";
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
      std::erase_if(Declaration->children, [](const auto &Part) {
        return Part->kind == K::ast_generic_parameter;
      });
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
    Node->text = OwnerModule.empty()
                     ? Owner->text + "." + Concrete
                     : OwnerModule + "." + Owner->text + "." + Concrete;
    Node->children.clear();
    Node->AssociatedOwnerArguments = 0;
    return true;
  }

  void Lower(std::unique_ptr<lex::Node> &Node, std::string_view Module) {
    const bool Scoped = Node->kind == K::ast_block ||
                        Node->kind == K::ast_block_expr ||
                        Node->kind == K::ast_class;
    if (Scoped) {
      AliasScopes.emplace_back();
      if (Node->kind == K::ast_class)
        for (const auto &Child : Node->children)
          if (Child->kind == K::ast_alias_decl)
            RegisterScopedAlias(*Child);
    }
    for (auto &Child : Node->children) {
      if (Child->kind != K::ast_alias_decl ||
          std::none_of(Child->children.begin(), Child->children.end(),
                       [](const auto &Part) {
                         return Part->kind == K::ast_generic_parameter;
                       }))
        Lower(Child, Module);
      if (Scoped && Node->kind != K::ast_class &&
          Child->kind == K::ast_alias_decl)
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
      for (auto Scope = AliasScopes.rbegin(); Scope != AliasScopes.rend();
           ++Scope)
        if (const auto Found = Scope->find(Name); Found != Scope->end()) {
          const auto *Alias = Found->second;
          std::vector<std::string> Parameters;
          for (const auto &Part : Alias->children)
            if (Part->kind == K::ast_generic_parameter)
              Parameters.push_back(Part->text);
          if (Parameters.size() != Node->children.size() ||
              std::find(ExpandingAliases.begin(), ExpandingAliases.end(),
                        Alias) != ExpandingAliases.end()) {
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
        Source = FindTemplate(std::string_view(Name).substr(0, Dot), Module,
                              &Node->Loc);
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
      Replacement->Loc.Len = Node->Loc.End() - Replacement->Loc.Offset;
      Node = std::move(Replacement);
    } else {
      Node->kind = K::ast_type;
      Node->text = Source->ModuleName.empty()
                       ? Concrete + MemberSuffix
                       : Source->ModuleName + "." + Concrete + MemberSuffix;
      Node->children.clear();
    }
  }

public:
  explicit GenericExpander(std::deque<SourceModule> &Modules)
      : Modules(Modules) {}

  bool Run() {
    for (const auto &Input : Modules)
      if (ModuleName(*Input.Parsed.root) == "std.meta") {
        MetaModule = Input.Parsed.root.get();
        break;
      }
    if (!MetaModule) {
      BuiltinMeta = lex::Lexer().parse(std::string(sema::BuiltinMetaSource),
                                       "std/meta/meta.kly");
      if (!BuiltinMeta->ok())
        return false;
      MetaModule = BuiltinMeta->root.get();
    }
    for (const auto &Input : Modules) {
      const auto Module = ModuleName(*Input.Parsed.root);
      for (const auto &Declaration : Input.Parsed.root->children) {
        if (Declaration->kind != K::ast_annotation_decl)
          continue;
        auto Definition = ReadAnnotationDefinition(*Declaration, Module);
        const auto Key = Module.empty() ? Declaration->text
                                        : Module + "." + Declaration->text;
        Annotations.emplace(Key, std::move(Definition));
      }
    }
    if (!Annotations.contains("std.annotation.final")) {
      BuiltinAnnotations =
          lex::Lexer().parse(std::string(sema::BuiltinAnnotationsSource),
                             "std/annotation/annotation.kly");
      if (!BuiltinAnnotations->ok())
        return false;
      for (const auto &Declaration : BuiltinAnnotations->root->children) {
        if (Declaration->kind != K::ast_annotation_decl)
          continue;
        Annotations.emplace(
            "std.annotation." + Declaration->text,
            ReadAnnotationDefinition(*Declaration, "std.annotation"));
      }
    }
    std::unordered_map<std::string, unsigned> Visit;
    std::function<void(const std::string &)> CheckComposition =
        [&](const std::string &Key) {
          if (Visit[Key] == 2 || !Valid)
            return;
          const auto &Definition = Annotations.at(Key);
          Visit[Key] = 1;
          if (Definition.Composition)
            for (const auto &Part : Definition.Composition->children) {
              const auto *Next =
                  FindAnnotation(Part->text, Definition.Module, Part->Loc);
              if (!Next) {
                Error(Part->Loc, "unknown composed annotation");
                return;
              }
              const auto NextKey =
                  Next->Module.empty()
                      ? Next->Declaration->text
                      : Next->Module + "." + Next->Declaration->text;
              if (NextKey == "std.annotation.cfg") {
                Error(Part->Loc, "@cfg cannot be composed");
                return;
              }
              if (Visit[NextKey] == 1) {
                Error(Part->Loc, "cyclic annotation composition");
                return;
              }
              CheckComposition(NextKey);
            }
          Visit[Key] = 2;
        };
    for (const auto &[Key, Definition] : Annotations)
      CheckComposition(Key);
    if (!Valid)
      return false;
    for (auto &Module : Modules) {
      const auto Name = ModuleName(*Module.Parsed.root);
      auto &Declarations = Module.Parsed.root->children;
      for (auto It = Declarations.begin(); It != Declarations.end();) {
        std::vector<std::string> Parameters;
        bool Pack = false;
        for (const auto &Child : (*It)->children)
          if (Child->kind == K::ast_generic_parameter ||
              Child->kind == K::ast_generic_pack) {
            if (Pack)
              Error(Child->Loc, "generic type pack must be last");
            Pack = Child->kind == K::ast_generic_pack;
            Parameters.push_back(Child->text);
          }
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
        if (Pack && (*It)->kind != K::ast_function)
          Error(Loc, "generic type packs are only supported on functions");
        Template Source{std::move(*It), &Module, Name, std::move(Parameters),
                        Pack};
        if (!Templates.emplace(Key, std::move(Source)).second)
          Error(Loc, "duplicate generic declaration");
        It = Declarations.erase(It);
      }
    }
    while (!Work.empty() && Valid) {
      const auto [Node, Module] = Work.front();
      Work.pop_front();
      if (Node->kind == K::ast_annotation_decl)
        continue;
      SourceModule *Source = nullptr;
      for (auto &Candidate : Modules)
        if (ModuleName(*Candidate.Parsed.root) == Module) {
          Source = &Candidate;
          break;
        }
      if (!Source || !ExpandAnnotations(*Node, Module, *Source))
        break;
      if (Node->kind == K::ast_class) {
        AliasScopes.emplace_back();
        for (const auto &Child : Node->children)
          if (Child->kind == K::ast_alias_decl)
            RegisterScopedAlias(*Child);
      }
      for (auto &Child : Node->children)
        Lower(Child, Module);
      if (Node->kind == K::ast_class)
        AliasScopes.pop_back();
    }
    for (auto &[Owner, Declarations] : PendingAliases)
      for (auto &Declaration : Declarations)
        Owner->children.push_back(std::move(Declaration));
    return Valid;
  }
};
} // namespace

bool kelyra::ExpandGenerics(std::deque<SourceModule> &Modules) {
  return GenericExpander(Modules).Run();
}
