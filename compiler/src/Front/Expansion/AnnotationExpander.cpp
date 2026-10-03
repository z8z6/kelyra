#include "Front/Expansion/AnnotationExpander.h"
#include "ExpansionInternal.h"
#include "Front/Parser/Parser.h"
#include "Support/BuiltinAnnotation.h"
#include "Support/Log.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_set>

using namespace kelyra;
using namespace kelyra::expansion;

namespace {
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

AnnotationDefinition ReadAnnotationDefinition(const lex::Node &Declaration, std::string Module) {
  AnnotationDefinition Definition;
  Definition.Declaration = &Declaration;
  Definition.Module = std::move(Module);
  for (const auto &Child : Declaration.children) {
    if (Child->kind == K::ast_annotation &&
        (Child->text == "target" || Child->text == "std.annotation.target") &&
        Child->children.size() == 1 && Child->children.front()->children.size() == 1) {
      const auto &Kind = *Child->children.front()->children.front();
      if (Kind.kind == K::ast_annotation_binding && Kind.children.size() == 1) {
        const auto Target = CalleeName(*Kind.children.front());
        const bool ClassTarget =
            Target == "Target.Class" || Target == "std.annotation.Target.Class";
        const bool FieldTarget =
            Target == "Target.Field" || Target == "std.annotation.Target.Field";
        if (ClassTarget || FieldTarget) {
          Definition.TargetBinding = Kind.text;
          Definition.FieldTarget = FieldTarget;
        }
      }
    }
    if (Child->kind == K::ast_annotation_uses)
      Definition.Composition = Child.get();
    if (Child->kind == K::ast_annotation_body)
      Definition.Body = Child.get();
  }
  return Definition;
}

} // namespace

class kelyra::AnnotationExpander::Implementation {
  const std::vector<const Module *> &Modules;
  ExpansionWorklist &Work;
  std::function<bool(std::string_view, std::string_view)> IsGeneric;
  std::unordered_map<std::string, AnnotationDefinition> Annotations;
  const lex::Node *MetaModule = nullptr;
  std::unordered_map<const lex::Node *, ClassSnapshot> ClassSnapshots;
  std::unordered_map<lex::Node *, std::vector<std::unique_ptr<lex::Node>>> PendingMethods;
  std::size_t NextAspectId = 0;
  bool Valid = true;

  void Error(const lex::Location &Loc, std::string_view Message) {
    kerr() << Loc.File << ':' << Loc.Line << ':' << Loc.Column << ": error: " << Message << '\n';
    Valid = false;
  }

  const AnnotationDefinition *FindAnnotation(std::string_view Name, std::string_view Module,
                                             const lex::Location &Loc) {
    const auto Find = [&](std::string_view Key) -> const AnnotationDefinition * {
      const auto It = Annotations.find(std::string(Key));
      return It == Annotations.end() ? nullptr : &It->second;
    };
    if (Name.find('.') != std::string_view::npos)
      return Find(Name);
    if (auto *Local = Find(Module.empty() ? std::string(Name)
                                          : std::string(Module) + "." + std::string(Name)))
      return Local;
    const AnnotationDefinition *Result = Find("std.annotation." + std::string(Name));
    for (const auto *Input : Modules) {
      if (Input->Name != Module)
        continue;
      for (const auto &Child : Input->Lex.root->children) {
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

  static void SubstituteValue(std::unique_ptr<lex::Node> &Node,
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

  static void SubstituteTargetName(std::unique_ptr<lex::Node> &Node, std::string_view Binding,
                                   std::string_view Target) {
    if (Node->kind == K::ast_name && Node->text == Binding)
      Node->text = Target;
    for (auto &Child : Node->children)
      SubstituteTargetName(Child, Binding, Target);
  }

  static void SetAnnotationOrigin(lex::Node &Node, std::string_view Module,
                                  std::string_view TargetType) {
    Node.AnnotationOriginModule = Module;
    if (Node.kind == K::ast_type && Node.text != TargetType && !Node.GenericArgument) {
      Node.GenericArgument = true;
      Node.GenericOriginModule = Module;
    }
    for (auto &Child : Node.children)
      SetAnnotationOrigin(*Child, Module, TargetType);
  }

  static void SubstituteField(std::unique_ptr<lex::Node> &Node, std::string_view Binding,
                              const lex::Node &Field, const lex::Node &FieldType,
                              std::string_view TargetModule) {
    if (Node->kind == K::ast_type && Node->text == std::string(Binding) + ".type") {
      Node = Clone(FieldType);
      BindArgument(*Node, TargetModule);
      return;
    }
    const bool Direct =
        (Node->kind == K::ast_name || Node->kind == K::ast_member) && Node->text == Binding;
    if (Direct) {
      Node->text = Field.text;
      if (Node->kind == K::ast_name)
        Node->BoundFieldName = true;
    }
    for (auto &Child : Node->children)
      SubstituteField(Child, Binding, Field, FieldType, TargetModule);
  }

  static std::optional<std::string>
  EvaluateNamePart(const lex::Node &Expression,
                   const std::unordered_map<std::string, const lex::Node *> &Values,
                   std::string_view Binding, std::string_view Target, unsigned Depth = 0) {
    if (Depth > 32)
      return std::nullopt;
    sema::ConstEvaluator Evaluator([&](const lex::Node &Node) -> std::optional<sema::ConstValue> {
      if (Node.kind == K::ast_name) {
        if (const auto Found = Values.find(Node.text); Found != Values.end()) {
          auto Value = EvaluateNamePart(*Found->second, Values, Binding, Target, Depth + 1);
          return Value ? std::optional<sema::ConstValue>(sema::ConstValue::String(*Value))
                       : std::nullopt;
        }
      }
      if (Node.kind == K::ast_member && Node.text == "name" && Node.children.size() == 1 &&
          Node.children.front()->kind == K::ast_name && Node.children.front()->text == Binding)
        return sema::ConstValue::String(std::string(Target));
      return std::nullopt;
    });
    auto Result = Evaluator.Evaluate(Expression);
    return Result && Result->Type == sema::ConstValue::Kind::String
               ? std::optional<std::string>(Result->Text)
               : std::nullopt;
  }

  bool InterpolateNames(lex::Node &Node,
                        const std::unordered_map<std::string, const lex::Node *> &Values,
                        std::string_view Binding, std::string_view Target) {
    if (Node.kind != K::ast_literal && Node.text.find("${") != std::string::npos) {
      if (Node.kind != K::ast_name && Node.kind != K::ast_member && Node.kind != K::ast_function &&
          Node.kind != K::ast_parameter && Node.kind != K::ast_field &&
          Node.kind != K::ast_const_field && Node.kind != K::ast_type &&
          Node.kind != K::ast_generic_type && Node.kind != K::ast_annotation) {
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
        auto Parsed = lex::Parser().parseExpression(Node.text.substr(Open + 2, Close - Open - 3));
        if (!Parsed.ok() || !Parsed.root) {
          Error(Node.Loc, "invalid identifier interpolation expression");
          return false;
        }
        auto Part = EvaluateNamePart(*Parsed.root, Values, Binding, Target);
        if (!Part) {
          Error(Node.Loc, "identifier interpolation requires a compile-time string");
          return false;
        }
        Result += *Part;
        Position = Close;
      }
      Result += Node.text.substr(Position);
      auto Parsed = lex::Parser().parseExpression(Result);
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
      if (Function->kind != K::ast_function || Callee != "std.meta." + Function->text)
        continue;
      for (const auto &Part : Function->children) {
        if (Part->kind != K::ast_annotation || !IsBuiltinAnnotation(Part->text, "intrinsic"))
          continue;
        if (Part->children.empty())
          return Function->text;
        if (Part->children.size() != 1 || Part->children.front()->children.size() != 1)
          return {};
        const auto &Value = Part->children.front()->children.front()->text;
        if (Value.size() >= 2 && Value.front() == '"' && Value.back() == '"')
          return Value.substr(1, Value.size() - 2);
      }
    }
    return {};
  }

  std::optional<bool> EvaluateAnnotationCondition(const lex::Node &Expression,
                                                  const ClassSnapshot *Snapshot) const {
    sema::ConstEvaluator Evaluator(
        [&](const lex::Node &Node) -> std::optional<sema::ConstValue> {
          if (!Snapshot || Node.kind != K::ast_meta || Node.children.size() != 1 ||
              Node.children.front()->kind != K::ast_type ||
              Node.children.front()->text != Snapshot->QualifiedName)
            return std::nullopt;
          return sema::ConstValue{sema::ConstValue::Kind::Symbol, "0"};
        },
        [&](const lex::Node &Callee) -> const lex::Node * {
          if (!Snapshot || !MetaModule || Callee.kind != K::ast_member ||
              Callee.children.size() != 1 || Callee.children.front()->kind != K::ast_meta ||
              Callee.children.front()->children.size() != 1 ||
              Callee.children.front()->children.front()->kind != K::ast_type ||
              Callee.children.front()->children.front()->text != Snapshot->QualifiedName)
            return nullptr;
          for (const auto Name : {"Class", "Symbol"})
            for (const auto &Class : MetaModule->children)
              if (Class->kind == K::ast_class && Class->text == Name)
                for (const auto &Method : Class->children)
                  if (Method->kind == K::ast_function && Method->text == Callee.text)
                    return Method.get();
          return nullptr;
        },
        [&](const lex::Node &Call,
            const std::vector<sema::ConstValue> &Args) -> std::optional<sema::ConstValue> {
          if (!Snapshot || Call.kind != K::ast_call || Call.children.empty() || Args.empty() ||
              Args.front().Type != sema::ConstValue::Kind::Integer || Args.front().Text != "0")
            return std::nullopt;
          const auto Operation = MetaOperation(CalleeName(*Call.children.front()));
          if (Args.size() == 1 && (Operation == "__read_public" || Operation == "meta.read_public"))
            return sema::ConstValue::Bool(Snapshot->Public);
          if (Args.size() != 2 || Args[1].Type != sema::ConstValue::Kind::String)
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
          if (!Snapshot || Owner.Type != sema::ConstValue::Kind::Symbol || Owner.Text != "0" ||
              Name != "name")
            return std::nullopt;
          const auto Dot = Snapshot->QualifiedName.rfind('.');
          return sema::ConstValue::String(
              Snapshot->QualifiedName.substr(Dot == std::string::npos ? 0 : Dot + 1));
        });
    auto Result = Evaluator.Evaluate(Expression);
    return Result && Result->Type == sema::ConstValue::Kind::Bool
               ? std::optional<bool>(Result->Text == "true")
               : std::nullopt;
  }

  bool ExpandUse(lex::Node &Target, const lex::Node &Use, std::string_view Module,
                 std::vector<std::string> &Stack, lex::Node *Owner, std::string_view TargetModule) {
    const auto *Definition = FindAnnotation(Use.text, Module, Use.Loc);
    if (!Definition || (!Definition->Composition && !Definition->Body))
      return Valid;
    const auto Key = Definition->Module.empty()
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
      if (Argument->kind != K::ast_annotation_argument || Argument->children.size() != 1)
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
        const auto *Component = FindAnnotation(Expanded->text, Definition->Module, Expanded->Loc);
        if (!Component) {
          Error(Expanded->Loc, "unknown composed annotation");
          return false;
        }
        Expanded->text = Component->Module.empty()
                             ? Component->Declaration->text
                             : Component->Module + "." + Component->Declaration->text;
        Expanded->AnnotationOriginModule = Definition->Module;
        auto *Generated = Expanded.get();
        Target.children.push_back(std::move(Expanded));
        if (!ExpandUse(Target, *Generated, Definition->Module, Stack, Owner, TargetModule))
          return false;
      }
    if (Definition->Body) {
      const bool FieldTarget = Definition->FieldTarget;
      if (Definition->TargetBinding.empty() ||
          (FieldTarget ? (!Owner || Owner->kind != K::ast_class ||
                          (Target.kind != K::ast_field && Target.kind != K::ast_const_field))
                       : Target.kind != K::ast_class)) {
        Error(Use.Loc, "annotation body requires a bound target");
        return false;
      }
      const lex::Node *FieldType = nullptr;
      if (FieldTarget)
        for (const auto &Part : Target.children)
          if (Part->kind == K::ast_type || Part->kind == K::ast_pointer_type ||
              Part->kind == K::ast_array_type || Part->kind == K::ast_slice_type ||
              Part->kind == K::ast_generic_type || Part->kind == K::ast_function_type)
            FieldType = Part.get();
      lex::Node Bound;
      std::unordered_map<std::string, const lex::Node *> Binding;
      if (!FieldTarget) {
        Bound.kind = K::ast_type;
        Bound.Loc = Target.Loc;
        Bound.text =
            TargetModule.empty() ? Target.text : std::string(TargetModule) + "." + Target.text;
        Binding.emplace(Definition->TargetBinding, &Bound);
      } else if (!FieldType) {
        Error(Use.Loc, "field annotation target has no type");
        return false;
      }
      std::vector<const lex::Node *> Members;
      std::function<bool(const lex::Node &, unsigned)> Select = [&](const lex::Node &Part,
                                                                    unsigned Depth) {
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
        const auto Found = ClassSnapshots.find(FieldTarget ? Owner : &Target);
        const ClassSnapshot *Snapshot = Found == ClassSnapshots.end() ? nullptr : &Found->second;
        const auto Result = EvaluateAnnotationCondition(*Condition, Snapshot);
        if (!Result) {
          Error(Part.children.front()->Loc, "annotation when requires a compile-time boolean");
          return false;
        }
        const lex::Node *Branch = *Result                     ? Part.children[1].get()
                                  : Part.children.size() == 3 ? Part.children[2].get()
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
        if ((!FieldTarget && Part->kind != K::ast_field && Part->kind != K::ast_function) ||
            (FieldTarget && Part->kind != K::ast_function)) {
          Error(Part->Loc, "annotation body may only inject fields and methods");
          return false;
        }
        auto Member = Clone(*Part);
        if (!InterpolateNames(*Member, Values, Definition->TargetBinding, Target.text))
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
        SubstituteField(Member, Definition->TargetBinding, Target, *FieldType, TargetModule);
        SetGeneratedLocation(*Member, Use.Loc);
        PendingMethods[Owner].push_back(std::move(Member));
      }
    }
    Stack.pop_back();
    return Valid;
  }

  bool ExpandAspects(lex::Node &Function, lex::Node *Owner, const kelyra::Module &Source,
                     std::string_view Module) {
    std::vector<std::string> Handlers;
    for (auto It = Function.children.begin(); It != Function.children.end();) {
      if ((*It)->kind != K::ast_annotation || !IsBuiltinAnnotation((*It)->text, "aspect")) {
        ++It;
        continue;
      }
      const auto &Use = **It;
      if (Use.children.size() != 1 || Use.children.front()->children.size() != 1) {
        Error(Use.Loc, "@aspect expects one function name");
        return false;
      }
      const auto &Handler = *Use.children.front()->children.front();
      if (Handler.kind != K::ast_meta || Handler.children.size() != 1) {
        Error(Use.Loc, "@aspect expects meta(function)");
        return false;
      }
      const auto &Target = *Handler.children.front();
      const auto Name = Target.kind == K::ast_type ? Target.text : CalleeName(Target);
      if (Name.empty()) {
        Error(Use.Loc, "@aspect expects a function name");
        return false;
      }
      Handlers.push_back(Name);
      It = Function.children.erase(It);
    }
    if (Handlers.empty())
      return true;
    const auto Block = std::find_if(Function.children.begin(),
                                    Function.children.end(),
                                    [](const auto &P) { return P->kind == K::ast_block; });
    if (Block == Function.children.end()) {
      Error(Function.Loc, "@aspect requires a function body");
      return false;
    }
    const bool Method = Owner && Owner->kind == K::ast_class;
    const bool Static =
        std::any_of(Function.children.begin(), Function.children.end(), [](const auto &P) {
          return P->kind == K::ast_annotation && IsBuiltinAnnotation(P->text, "static");
        });
    const bool Receiver = Method && !Static;
    std::vector<const lex::Node *> Parameters;
    const lex::Node *Return = nullptr;
    for (const auto &Part : Function.children) {
      if (Part->kind == K::ast_parameter)
        Parameters.push_back(Part.get());
      if (Part->kind == K::ast_type || Part->kind == K::ast_pointer_type ||
          Part->kind == K::ast_array_type || Part->kind == K::ast_slice_type ||
          Part->kind == K::ast_generic_type || Part->kind == K::ast_function_type)
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
    for (auto Handler = Handlers.rbegin(); Handler != Handlers.rend(); ++Handler) {
      const auto Id = std::to_string(NextAspectId++);
      const std::string OriginalName = "__aspect_original_" + Id;
      const std::string ThunkName = "__aspect_thunk_" + Id;
      auto Original = Clone(Function);
      Original->text = OriginalName;
      Original->children.erase(
          std::remove_if(Original->children.begin(),
                         Original->children.end(),
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
        Source.Lex.root->children.push_back(std::move(Original));
        Work.emplace_back(Generated, &Source);
      }

      std::string Call = Method ? (Receiver ? "__aspect_receiver." : Owner->text + ".") : "";
      Call += OriginalName + "(";
      std::string Thunk = "fn " + ThunkName + "(context: *u8) -> " + ResultType + " {\n";
      const auto Count = Parameters.size() + (Receiver ? 1 : 0);
      Thunk += "let __aspect_slots = context as *[" +
               std::to_string(std::max<std::size_t>(Count, 1)) + "]usize;\n";
      if (Receiver)
        Thunk += "let __aspect_receiver = (*__aspect_slots)[0] as *" + Owner->text + ";\n";
      for (std::size_t I = 0; I < Parameters.size(); ++I) {
        if (I)
          Call += ", ";
        Call += "*(((*__aspect_slots)[" + std::to_string(I + (Receiver ? 1 : 0)) + "]) as *" +
                ParameterTypes[I] + ")";
      }
      Call += ")";
      Thunk += Void ? Call + ";\nreturn;\n}\n" : "return " + Call + ";\n}\n";
      auto ParsedThunk = lex::Parser().parse(Thunk, Function.Loc.File);
      if (!ParsedThunk.ok() || ParsedThunk.root->children.empty()) {
        Error(Function.Loc, "cannot generate aspect continuation");
        return false;
      }
      auto Continuation = std::move(ParsedThunk.root->children.front());
      SetGeneratedLocation(*Continuation, Function.Loc);
      auto *ContinuationNode = Continuation.get();
      Source.Lex.root->children.push_back(std::move(Continuation));
      Work.emplace_back(ContinuationNode, &Source);

      std::string Body = "fn __wrapper() -> void {\n";
      Body +=
          "let __aspect_context: [" + std::to_string(std::max<std::size_t>(Count, 1)) + "]usize;\n";
      if (Receiver)
        Body += "__aspect_context[0] = this as usize;\n";
      for (std::size_t I = 0; I < Parameters.size(); ++I)
        Body += "__aspect_context[" + std::to_string(I + (Receiver ? 1 : 0)) + "] = (&" +
                Parameters[I]->text + ") as usize;\n";
      Body += "let __aspect_invocation = std.aspect.";
      Body += Void ? "VoidInvocation" : "Invocation<" + ResultType + ">";
      Body += "((&__aspect_context[0]) as *u8, " + ThunkName + ");\n";
      std::string AspectCall = *Handler;
      if (!Void && IsGeneric(*Handler, Module))
        AspectCall += "<" + ResultType + ">";
      AspectCall += "(&__aspect_invocation)";
      Body += Void ? AspectCall + ";\nreturn;\n}\n" : "return " + AspectCall + ";\n}\n";
      auto ParsedBody = lex::Parser().parse(Body, Function.Loc.File);
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

  bool ExpandAnnotations(lex::Node &Node, std::string_view Module, const kelyra::Module &Source,
                         lex::Node *Owner = nullptr) {
    if (Node.kind == K::ast_class) {
      ClassSnapshot Snapshot;
      Snapshot.QualifiedName = Module.empty() ? Node.text : std::string(Module) + "." + Node.text;
      for (const auto &Child : Node.children) {
        if (Child->kind == K::ast_public)
          Snapshot.Public = true;
        else if (Child->kind == K::ast_field || Child->kind == K::ast_const_field)
          Snapshot.Fields.insert(Child->text);
        else if (Child->kind == K::ast_function || Child->kind == K::ast_constructor ||
                 Child->kind == K::ast_destructor)
          Snapshot.Functions.insert(Child->text);
      }
      ClassSnapshots.emplace(&Node, std::move(Snapshot));
    }
    const bool Field = Node.kind == K::ast_field || Node.kind == K::ast_const_field;
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
        if (Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "static"))
          Static = Part.get();
      if (Static)
        for (std::size_t I = Before; I < PendingMethods[Owner].size(); ++I)
          PendingMethods[Owner][I]->children.insert(PendingMethods[Owner][I]->children.begin(),
                                                    Clone(*Static));
    }
    if (Node.kind == K::ast_function && !ExpandAspects(Node, Owner, Source, Module))
      return false;
    for (auto &Child : Node.children)
      if (Child->kind == K::ast_field || Child->kind == K::ast_const_field ||
          Child->kind == K::ast_function || Child->kind == K::ast_constructor ||
          Child->kind == K::ast_destructor)
        if (!ExpandAnnotations(*Child, Module, Source, &Node))
          return false;
    if (Node.kind == K::ast_class) {
      if (auto Found = PendingMethods.find(&Node); Found != PendingMethods.end()) {
        for (auto &Method : Found->second)
          Node.children.push_back(std::move(Method));
        PendingMethods.erase(Found);
      }
    }
    return true;
  }

public:
  Implementation(const std::vector<const Module *> &Modules, ExpansionWorklist &Work,
                 std::function<bool(std::string_view, std::string_view)> IsGeneric)
      : Modules(Modules), Work(Work), IsGeneric(std::move(IsGeneric)) {}

  bool Initialize() {
    for (const auto *Input : Modules)
      if (Input->Name == "std.meta") {
        MetaModule = Input->Lex.root.get();
        break;
      }
    if (!MetaModule) {
      kerr() << "annotation expansion requires std.meta to be loaded\n";
      return false;
    }
    for (const auto *Input : Modules) {
      const auto Module = Input->Name;
      for (const auto &Declaration : Input->Lex.root->children) {
        if (Declaration->kind != K::ast_annotation_decl)
          continue;
        auto Definition = ReadAnnotationDefinition(*Declaration, Module);
        const auto Key = Module.empty() ? Declaration->text : Module + "." + Declaration->text;
        Annotations.emplace(Key, std::move(Definition));
      }
    }
    std::unordered_map<std::string, unsigned> Visit;
    std::function<void(const std::string &)> CheckComposition = [&](const std::string &Key) {
      if (Visit[Key] == 2 || !Valid)
        return;
      const auto &Definition = Annotations.at(Key);
      Visit[Key] = 1;
      if (Definition.Composition)
        for (const auto &Part : Definition.Composition->children) {
          const auto *Next = FindAnnotation(Part->text, Definition.Module, Part->Loc);
          if (!Next) {
            Error(Part->Loc, "unknown composed annotation");
            return;
          }
          const auto NextKey = Next->Module.empty() ? Next->Declaration->text
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
    return Valid;
  }

  bool Expand(lex::Node &Node, const Module &Source) {
    return ExpandAnnotations(Node, Source.Name, Source) && Valid;
  }
};

AnnotationExpander::AnnotationExpander(
    const std::vector<const Module *> &Modules, ExpansionWorklist &Work,
    std::function<bool(std::string_view, std::string_view)> IsGeneric)
    : Impl(std::make_unique<Implementation>(Modules, Work, std::move(IsGeneric))) {}
AnnotationExpander::~AnnotationExpander() = default;
bool AnnotationExpander::Initialize() { return Impl->Initialize(); }
bool AnnotationExpander::Expand(lex::Node &Node, const Module &Source) {
  return Impl->Expand(Node, Source);
}
