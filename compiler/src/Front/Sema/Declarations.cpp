#include "Front/Sema/Sema.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include <algorithm>
#include <functional>
#include <sstream>
using namespace kelyra;

namespace {
std::size_t IntrinsicArity(std::string_view Operation) {
  if (Operation == "__has_annotation" || Operation == "meta.has_annotation")
    return 2;
  if (Operation == "__has_member" || Operation == "__has_field" || Operation == "__has_function")
    return 2;
  for (const auto Name : {"__read_public",
                          "__has_constructor",
                          "__has_default_constructor",
                          "__has_destructor",
                          "__has_base_class",
                          "__is_interface",
                          "__is_final",
                          "__is_static",
                          "__is_method",
                          "__is_constructor",
                          "__is_destructor",
                          "meta.read_public"})
    if (Operation == Name)
      return 1;
  return 0;
}

bool ValidForwardConstructor(const lex::Node &Constructor) {
  using K = lex::NodeKind;
  const lex::Node *Generic = nullptr;
  const lex::Node *Parameter = nullptr;
  for (const auto &Child : Constructor.children) {
    if (Child->kind == K::ast_generic_pack) {
      if (Generic)
        return false;
      Generic = Child.get();
    } else if (Child->kind == K::ast_parameter_pack) {
      if (Parameter)
        return false;
      Parameter = Child.get();
    } else if (Child->kind == K::ast_generic_parameter || Child->kind == K::ast_parameter)
      return false;
  }
  return Generic && Parameter && !Parameter->children.empty() &&
         Parameter->children.back()->kind == K::ast_type &&
         Parameter->children.back()->text == Generic->text &&
         std::any_of(Parameter->children.begin(), Parameter->children.end(), [](const auto &Child) {
           return Child->kind == K::ast_annotation && IsBuiltinAnnotation(Child->text, "forward");
         });
}

std::string Mangle(std::string_view Module, std::string_view Name, bool IsEntrypoint) {
  if (IsEntrypoint)
    return "main";
  if (Module.empty())
    return std::string(Name);
  std::ostringstream Result;
  Result << "_K";
  std::size_t Start = 0;
  while (Start < Module.size()) {
    const auto End = Module.find('.', Start);
    const auto Part =
        Module.substr(Start, End == std::string_view::npos ? Module.size() - Start : End - Start);
    Result << Part.size() << Part;
    if (End == std::string_view::npos)
      break;
    Start = End + 1;
  }
  Result << 'F' << Name.size() << Name;
  return Result.str();
}

} // namespace

bool sema::Sema::RegisterFunctions(const std::vector<ModuleInput> &Modules) {
  using K = lex::NodeKind;
  for (const auto &External : ExternalFunctions) {
    FunctionInfo Info;
    Info.Module = "c";
    Info.Symbol = External.Name;
    Info.Parameters = External.Parameters;
    Info.Return = External.Return;
    Info.Public = true;
    Info.Variadic = External.Variadic;
    Info.External = &External;
    if (!Functions.emplace("c." + External.Name, std::move(Info)).second && !Modules.empty())
      Diagnostics.push_back({lex::DiagnosticKind::DuplicateFunction, Modules.front().Ast->Loc});
  }

  for (const auto &Input : Modules) {
    const auto &Module = *Input.Ast;
    const auto Name = Input.Name;
    Context.CurrentModule = Name;
    const auto RegisterFunction = [&](const lex::Node &Function, std::string_view Owner) {
      for (const auto &Part : Function.children)
        if (Part->kind == K::ast_parameter_pack || Part->kind == K::ast_generic_pack)
          Error(*Part, lex::DiagnosticKind::UnsupportedType);
      FunctionInfo Info;
      Info.Node = &Function;
      Info.Module = Name;
      Info.Public = IsPublic(Function);
      Info.OwnerClass = Owner;
      Info.Static =
          std::any_of(Function.children.begin(), Function.children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "static");
          });
      if (Info.Static &&
          (Owner.empty() || Function.kind != K::ast_function || Function.text == "copy" ||
           Function.text == "move" || Classes.at(std::string(Owner)).IsInterface))
        Error(Function, lex::DiagnosticKind::InvalidClass);
      Info.Virtual =
          std::any_of(Function.children.begin(), Function.children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "virtual");
          });
      Info.Override =
          std::any_of(Function.children.begin(), Function.children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "override");
          });
      const auto LocalName =
          Owner.empty() ? Function.text
                        : std::string(Owner.substr(Owner.rfind('.') + 1)) + "." + Function.text;
      const bool MainAnnotation =
          std::any_of(Function.children.begin(), Function.children.end(), [](const auto &Part) {
            return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "main");
          });
      if (Owner.empty() && MainAnnotation)
        EntrypointCandidates.push_back(&Function);
      Info.Symbol = Mangle(Name, LocalName, Owner.empty() && MainAnnotation);
      if (Owner.empty() && Input.IsEntry && !MainAnnotation && Function.text == "main" &&
          Name.empty())
        Info.Symbol = "_K0F4main";
      const lex::Node *ExternAnnotation = nullptr;
      const lex::Node *IntrinsicAnnotation = nullptr;
      const lex::Node *CallConvAnnotation = nullptr;
      bool HasBody = false;
      for (const auto &Part : Function.children) {
        if (Part->kind == K::ast_block)
          HasBody = true;
        if (Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "extern")) {
          if (ExternAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          ExternAnnotation = Part.get();
        }
        if (Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "intrinsic")) {
          if (IntrinsicAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          IntrinsicAnnotation = Part.get();
        }
        if (Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "callconv")) {
          if (CallConvAnnotation)
            Error(*Part, lex::DiagnosticKind::DuplicateAnnotation);
          CallConvAnnotation = Part.get();
        }
      }
      if (IntrinsicAnnotation) {
        const auto Operation = detail::IntrinsicOperation(Function, *IntrinsicAnnotation);
        const bool MemoryIntrinsic =
            Name == "std.memory" && Owner.empty() && Operation &&
            (Operation->starts_with("init_copy__G") || Operation->starts_with("init_move__G") ||
             Operation->starts_with("assume_init__G") || Operation->starts_with("drop_init__G"));
        if ((!MemoryIntrinsic &&
             (Name != "std.meta" || HasBody || !Operation || !IntrinsicArity(*Operation))) ||
            !Owner.empty() || MainAnnotation || ExternAnnotation || CallConvAnnotation) {
          Error(*IntrinsicAnnotation, lex::DiagnosticKind::InvalidIntrinsicDeclaration);
        }
      }
      if (ExternAnnotation) {
        if (!Owner.empty() || HasBody || MainAnnotation)
          Error(*ExternAnnotation, lex::DiagnosticKind::InvalidExternDeclaration);
        Info.Symbol = Function.text;
        if (ExternAnnotation->children.empty() || ExternAnnotation->children.size() > 2)
          Error(*ExternAnnotation, lex::DiagnosticKind::InvalidExternDeclaration);
        for (std::size_t Index = 0; Index < ExternAnnotation->children.size(); ++Index) {
          const auto &Argument = *ExternAnnotation->children[Index];
          if (Argument.kind != K::ast_annotation_argument || !Argument.text.empty() ||
              Argument.children.size() != 1 || Argument.children.front()->kind != K::ast_literal) {
            Error(*ExternAnnotation, lex::DiagnosticKind::InvalidExternDeclaration);
            continue;
          }
          const auto &Spelling = Argument.children.front()->text;
          if (Spelling.size() < 3 || Spelling.front() != '"' || Spelling.back() != '"' ||
              Spelling.find('\\') != std::string::npos) {
            Error(*ExternAnnotation, lex::DiagnosticKind::InvalidExternDeclaration);
            continue;
          }
          if (Index == 0)
            Info.Symbol = Spelling.substr(1, Spelling.size() - 2);
          else
            Info.Library = Spelling.substr(1, Spelling.size() - 2);
        }
      } else if (!HasBody && !IntrinsicAnnotation && !Input.IsExternal &&
                 (Owner.empty() || !Classes.at(std::string(Owner)).IsInterface)) {
        Error(Function, lex::DiagnosticKind::InvalidExternDeclaration);
      }
      Info.Abstract = !HasBody && !Owner.empty() && Classes.at(std::string(Owner)).IsInterface;
      if (CallConvAnnotation) {
        if (!ExternAnnotation || !Owner.empty()) {
          Error(*CallConvAnnotation, lex::DiagnosticKind::InvalidExternDeclaration);
        }
      }
      if (!Owner.empty() && !Info.Static) {
        Type Receiver{BuiltinType::Class, {}};
        Receiver.ClassName = Owner;
        Receiver.AddPointer();
        Info.Parameters.push_back(std::move(Receiver));
      }
      const auto Kind = Owner.empty()                         ? MetaKind::Function
                        : Function.kind == K::ast_constructor ? MetaKind::Constructor
                        : Function.kind == K::ast_destructor  ? MetaKind::Destructor
                                                              : MetaKind::Method;
      const auto Id = RegisterMetaDeclaration(Function, Kind, Name, Info.Public);
      if (!Owner.empty()) {
        Reflection.Records[Id].QualifiedName = std::string(Owner) + "." + Function.text;
        const auto Parent = Reflection.GetId(*Classes.at(std::string(Owner)).Node);
        if (Parent)
          Reflection.Records[*Parent].Children.push_back(Id);
      }
      Reflection.Records[Id].Static = Info.Static;
      const auto IsInterfacePointer = [&](const Type &Value) {
        if (!Value.IsPointer() || Value.PointerDepth != 1 || Value.Element != BuiltinType::Class)
          return false;
        const auto *Class = GetClass(Value.ClassName);
        return Class && Class->IsInterface;
      };
      for (const auto &Part : Function.children) {
        if (Part->kind == K::ast_parameter && !Part->children.empty() &&
            detail::IsTypeNode(Part->children.back()->kind)) {
          if (auto Parameter = CheckType(*Part->children.back())) {
            Info.Parameters.push_back(*Parameter);
            Types[Part.get()] = *Parameter;
            if (ExternAnnotation && Parameter->IsSlice())
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (ExternAnnotation && !Parameter->IsPointer() && !Parameter->IsFunction() &&
                !IsNumeric(Parameter->Element) && Parameter->Element != BuiltinType::Bool &&
                Parameter->Element != BuiltinType::CBool && Parameter->Element != BuiltinType::Char)
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (Parameter->IsClass() && ExternAnnotation)
              Error(*Part, lex::DiagnosticKind::ClassValueOperation);
            if (ExternAnnotation && IsInterfacePointer(*Parameter))
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (Parameter->IsVoid() || Parameter->IsResults())
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            const auto ParameterId =
                RegisterMetaDeclaration(*Part, MetaKind::Parameter, Name, false);
            Reflection.Records[ParameterId].QualifiedName =
                Reflection.Records[Id].QualifiedName + "." + Part->text;
            Reflection.Records[ParameterId].Type = GetOrCreateMetaType(*Parameter);
            Reflection.Records[Id].Children.push_back(ParameterId);
          }
        } else if (detail::IsTypeNode(Part->kind)) {
          if (auto Return = CheckType(*Part)) {
            Info.Return = *Return;
            if (ExternAnnotation && Return->IsSlice())
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (ExternAnnotation && !Return->IsVoid() && !Return->IsPointer() &&
                !Return->IsFunction() && !IsNumeric(Return->Element) &&
                Return->Element != BuiltinType::Bool && Return->Element != BuiltinType::CBool &&
                Return->Element != BuiltinType::Char)
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
            if (ExternAnnotation && IsInterfacePointer(*Return))
              Error(*Part, lex::DiagnosticKind::UnsupportedType);
          }
        }
      }
      if (IntrinsicAnnotation) {
        const auto Operation = detail::IntrinsicOperation(Function, *IntrinsicAnnotation);
        const bool MemoryIntrinsic =
            Name == "std.memory" && Owner.empty() && Operation &&
            (Operation->starts_with("init_copy__G") || Operation->starts_with("init_move__G") ||
             Operation->starts_with("assume_init__G") || Operation->starts_with("drop_init__G"));
        const auto Arity = Operation ? IntrinsicArity(*Operation) : 0;
        const bool ValidReturn = Info.Return == Type{BuiltinType::Bool, {}};
        const bool NamedMemberQuery =
            Operation && (*Operation == "__has_member" || *Operation == "__has_field" ||
                          *Operation == "__has_function");
        const bool ValidParameters =
            Arity != 0 && Info.Parameters.size() == Arity &&
            Info.Parameters.front() == Type{BuiltinType::USize, {}} &&
            (Arity == 1 ||
             (NamedMemberQuery ? Info.Parameters[1].IsReadOnlySlice() &&
                                     Info.Parameters[1].Indexed() == Type{BuiltinType::U8, {}}
                               : Info.Parameters[1] == Type{BuiltinType::USize, {}}));
        if (!MemoryIntrinsic && (!Arity || !ValidReturn || !ValidParameters)) {
          Error(*IntrinsicAnnotation, lex::DiagnosticKind::InvalidIntrinsicDeclaration);
        }
      }
      Reflection.Records[Id].Type = GetOrCreateMetaType(Info.Return);
      const auto Key = Owner.empty() ? (Name.empty() ? Function.text : Name + "." + Function.text)
                                     : std::string(Owner) + "." + Function.text;
      Info.QualifiedName = Key;
      Info.Signature =
          Function.text + "#" +
          EncodeOverloadParameters(Info.Parameters, Owner.empty() || Info.Static ? 0 : 1);
      auto &Group = FunctionGroups[Key];
      if (Classes.contains(Key) || Enums.contains(Key) || TypeDeclarations.contains(Key) ||
          std::any_of(Group.begin(),
                      Group.end(),
                      [&](const auto &Existing) {
                        return Functions.at(Existing).Signature == Info.Signature;
                      }) ||
          (!Group.empty() && (Function.kind != K::ast_function || Function.text == "init" ||
                              Function.text == "deinit" || Function.text == "copy" ||
                              Function.text == "move" || MainAnnotation))) {
        Error(Function, lex::DiagnosticKind::DuplicateFunction);
        return;
      }
      const auto StorageKey = Group.empty() ? Key : Key + "#" + Info.Signature;
      Info.BaseSymbol = Info.Symbol;
      Info.Extern = ExternAnnotation != nullptr;
      Info.SpecialAbi = MainAnnotation || IntrinsicAnnotation || Info.Symbol == "_K0F4main" ||
                        (!Owner.empty() && (Function.text == "init" || Function.text == "deinit" ||
                                            Function.text == "copy" || Function.text == "move"));
      Reflection.Records[Id].Symbol = Info.Symbol;
      Types[&Function] = Info.Return;
      Functions.emplace(StorageKey, Info);
      Group.push_back(StorageKey);
      FunctionKeys[&Function] = StorageKey;
      Symbols[&Function] = Info.Symbol;
    };
    for (const auto &Child : Module.children) {
      if (Child->kind == K::ast_module_decl)
        continue;
      if (Child->kind == K::ast_import)
        continue;
      if (Child->kind == K::ast_alias_decl || Child->kind == K::ast_enum)
        continue;
      if (Child->kind == K::ast_annotation_decl) {
        RegisterMetaDeclaration(*Child, MetaKind::Annotation, Name, IsPublic(*Child));
        RegisterAnnotation(*Child, Name);
        continue;
      }
      if (Child->kind == K::ast_class) {
        const auto ClassIt = Classes.find(Name.empty() ? Child->text : Name + "." + Child->text);
        if (ClassIt == Classes.end() || ClassIt->second.Node != Child.get())
          continue;
        auto &Class = ClassIt->second;
        Context.CurrentClass = Class.QualifiedName;
        std::unordered_map<std::string, bool> MemberNames;
        for (const auto &Member : Child->children) {
          if (Member->kind == K::ast_public || Member->kind == K::ast_annotation ||
              Member->kind == K::ast_base_type)
            continue;
          const bool IsMethod = Member->kind == K::ast_function;
          const auto [Existing, NewName] = MemberNames.emplace(Member->text, IsMethod);
          if ((!NewName && (!IsMethod || !Existing->second)) ||
              (Member->kind == K::ast_function &&
               (Member->text == "init" || Member->text == "deinit")))
            Error(*Member, lex::DiagnosticKind::InvalidClass);
          if (Member->kind == K::ast_field) {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            const auto TypeNode =
                std::find_if(Member->children.begin(),
                             Member->children.end(),
                             [](const auto &Part) { return detail::IsTypeNode(Part->kind); });
            if (TypeNode == Member->children.end())
              continue;
            if (auto Value = CheckType(**TypeNode)) {
              if (Value->IsVoid() || Value->IsResults())
                Error(*Member, lex::DiagnosticKind::UnsupportedType);
              Types[Member.get()] = *Value;
              const bool Static = std::any_of(
                  Member->children.begin(), Member->children.end(), [](const auto &Part) {
                    return Part->kind == K::ast_annotation &&
                           IsBuiltinAnnotation(Part->text, "static");
                  });
              if (Static) {
                if (!CanZeroInitialize(*Value))
                  Error(*Member, lex::DiagnosticKind::InvalidEnumInitialization);
                auto Element = *Value;
                while (Element.IsArray())
                  Element = Element.Indexed();
                if (Element.IsClass() || Element.IsRecord() || Element.IsFunction() ||
                    !GetBitWidth(Element))
                  Error(*Member, lex::DiagnosticKind::UnsupportedType);
                Class.StaticFields.push_back(
                    {Member.get(),
                     Member->text,
                     *Value,
                     Mangle(Name, Class.Name + "." + Member->text + ".static", false),
                     IsPublic(*Member)});
              } else {
                Class.Fields.push_back({Member.get(), Member->text, *Value, IsPublic(*Member)});
              }
              const auto Id =
                  RegisterMetaDeclaration(*Member, MetaKind::Field, Name, IsPublic(*Member));
              Reflection.Records[Id].QualifiedName = Class.QualifiedName + "." + Member->text;
              Reflection.Records[Id].Type = GetOrCreateMetaType(*Value);
              Reflection.Records[Id].Static = Static;
              Reflection.Records[*Reflection.GetId(*Child)].Children.push_back(Id);
            }
            continue;
          }
          if (Member->kind == K::ast_const_field) {
            if (!Class.IsInterface || Member->children.size() != 2 ||
                !EvaluateConstant(*Member->children[1]))
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            if (Member->children.size() == 2) {
              auto Value = CheckType(*Member->children[0]);
              if (Value && (!IsNumeric(Value->Element) && Value->Element != BuiltinType::Bool &&
                            !(Value->IsPointer() && Value->Element == BuiltinType::CChar &&
                              Member->children[1]->kind == K::ast_literal)))
                Error(*Member, lex::DiagnosticKind::UnsupportedType);
              if (Value) {
                CheckExpression(*Member->children[1], *Value);
                Class.Constants.push_back({Member.get(),
                                           Member->children[1].get(),
                                           Member->text,
                                           *Value,
                                           IsPublic(*Member)});
                Types[Member.get()] = *Value;
                const auto Id =
                    RegisterMetaDeclaration(*Member, MetaKind::Field, Name, IsPublic(*Member));
                Reflection.Records[Id].QualifiedName = Class.QualifiedName + "." + Member->text;
                Reflection.Records[Id].Type = GetOrCreateMetaType(*Value);
                Reflection.Records[Id].Static = true;
                Reflection.Records[*Reflection.GetId(*Child)].Children.push_back(Id);
              }
            }
            continue;
          }
          if (Member->kind != K::ast_function && Member->kind != K::ast_constructor &&
              Member->kind != K::ast_destructor)
            continue;
          if (Member->kind == K::ast_constructor) {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            if (Class.Constructor)
              Error(*Member, lex::DiagnosticKind::DuplicateFunction);
            Class.Constructor = Member.get();
            if (std::any_of(Member->children.begin(), Member->children.end(), [](const auto &Part) {
                  return Part->kind == K::ast_generic_pack;
                })) {
              if (!ValidForwardConstructor(*Member))
                Error(*Member, lex::DiagnosticKind::InvalidClass);
              continue;
            }
          } else if (Member->kind == K::ast_destructor) {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            if (Class.Destructor || IsPublic(*Member) ||
                std::any_of(Member->children.begin(), Member->children.end(), [](const auto &Part) {
                  return Part->kind == K::ast_parameter;
                }))
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Destructor = Member.get();
          } else if (Member->kind == K::ast_function && Member->text == "copy") {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Copy = Member.get();
          } else if (Member->kind == K::ast_function && Member->text == "move") {
            if (Class.IsInterface)
              Error(*Member, lex::DiagnosticKind::InvalidClass);
            Class.Move = Member.get();
          }
          RegisterFunction(*Member, Class.QualifiedName);
        }
        auto &MetaMembers = Reflection.Records[*Reflection.GetId(*Child)].Children;
        std::stable_sort(MetaMembers.begin(), MetaMembers.end(), [&](MetaId Left, MetaId Right) {
          return Reflection.Get(Left).Loc.Begin < Reflection.Get(Right).Loc.Begin;
        });
        Class.UserFieldCount = Class.Fields.size();
        for (const auto &Member : Child->children) {
          if (Member->kind != K::ast_function)
            continue;
          const auto *Info = FindFunction(*Member);
          if (!Info || !Info->Virtual || Info->Static)
            continue;
          if (Class.IsInterface || Info->Override || Member->text == "copy" ||
              Member->text == "move") {
            Error(*Member, lex::DiagnosticKind::InvalidClass);
            continue;
          }
          Type Slot{BuiltinType::Function, {}};
          Slot.Parameters = Info->Parameters;
          Slot.Results.push_back(Info->Return);
          Class.VirtualSlots.emplace(Info->Signature, Class.Fields.size());
          Class.Fields.push_back({Member.get(), "$virtual." + Info->Signature, Slot, false});
        }
        if (Class.IsInterface) {
          Class.DefaultConstructible = false;
          Context.CurrentClass.clear();
          continue;
        }
        if (Class.Constructor) {
          Class.ConstructorSymbol =
              Symbols.contains(Class.Constructor) ? Symbols.at(Class.Constructor) : std::string();
          Class.DefaultConstructible =
              !Class.ConstructorSymbol.empty() &&
              std::none_of(Class.Constructor->children.begin(),
                           Class.Constructor->children.end(),
                           [](const auto &Part) { return Part->kind == K::ast_parameter; });
        } else {
          // No explicit init: synthesize a default constructor named like an
          // explicit one so a class without init still constructs.
          Class.ConstructorSymbol = Mangle(Name, Class.Name + ".init", false);
          Class.DefaultConstructible = true;
        }
        if (Class.Destructor)
          Class.DestructorSymbol = Symbols.at(Class.Destructor);
        else
          Class.DestructorSymbol = Mangle(Name, Class.Name + ".deinit", false);
        const auto ValidateTransfer = [&](const lex::Node *Method,
                                          std::string_view MethodName,
                                          std::string &Symbol) {
          if (!Method) {
            Symbol = Mangle(Name, Class.Name + "." + std::string(MethodName), false);
            return;
          }
          const auto &Info = Functions.at(Class.QualifiedName + "." + std::string(MethodName));
          Type Source{BuiltinType::Class, {}};
          Source.ClassName = Class.QualifiedName;
          Source.AddPointer();
          if (Info.Parameters.size() != 2 || Info.Parameters[1] != Source || !Info.Return.IsVoid())
            Error(*Method, lex::DiagnosticKind::InvalidClass);
          Symbol = Info.Symbol;
        };
        ValidateTransfer(Class.Copy, "copy", Class.CopySymbol);
        ValidateTransfer(Class.Move, "move", Class.MoveSymbol);
        Context.CurrentClass.clear();
        continue;
      }
      if (Child->kind != K::ast_function) {
        Error(*Child, lex::DiagnosticKind::UnsupportedDeclaration);
        continue;
      }
      RegisterFunction(*Child, {});
    }
  }

  // Registration can leave malformed declarations without function metadata.
  // Stop before class and interface passes inspect those declarations.
  if (!Diagnostics.empty())
    return false;

  // Every ordinary function uses its parameter types in the exported symbol.
  // Adding an overload therefore preserves symbols of existing declarations.
  for (const auto &[Name, Group] : FunctionGroups) {
    for (const auto &Key : Group) {
      auto &Info = Functions.at(Key);
      if (!Info.Extern && !Info.SpecialAbi)
        Info.Symbol = Info.BaseSymbol + "__O" +
                      EncodeOverloadParameters(Info.Parameters,
                                               Info.OwnerClass.empty() || Info.Static ? 0 : 1);
      Symbols[Info.Node] = Info.Symbol;
      if (const auto Id = Reflection.GetId(*Info.Node))
        Reflection.Records[*Id].Symbol = Info.Symbol;
    }
  }
  std::unordered_map<std::string, const lex::Node *> ExportedSymbols;
  for (const auto &[Name, Info] : Functions) {
    if (!Info.Node)
      continue;
    // CheckEntrypoint diagnoses multiple @main declarations separately.
    if (Info.SpecialAbi && Info.Symbol == "main")
      continue;
    if (const auto [It, New] = ExportedSymbols.emplace(Info.Symbol, Info.Node); !New)
      Error(*Info.Node, lex::DiagnosticKind::DuplicateFunction);
  }

  return Diagnostics.empty();
}
