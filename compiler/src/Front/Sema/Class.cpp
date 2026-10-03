#include "Front/Sema/Sema.h"
#include "SemaInternal.h"
#include "Support/BuiltinAnnotation.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>

using namespace kelyra;
using K = lex::NodeKind;

namespace {
bool IsNumericConstant(const lex::Node &Value) {
  if (Value.kind == K::ast_literal)
    return !Value.text.empty() && Value.text.front() >= '0' && Value.text.front() <= '9';
  if (Value.kind == K::ast_group && Value.children.size() == 1)
    return IsNumericConstant(*Value.children.front());
  if (Value.kind == K::ast_unary && Value.children.size() == 1 &&
      (Value.text == "+" || Value.text == "-"))
    return IsNumericConstant(*Value.children.front());
  if (Value.kind == K::ast_binary && Value.children.size() == 2 &&
      (Value.text == "+" || Value.text == "-" || Value.text == "*" || Value.text == "/" ||
       Value.text == "%"))
    return IsNumericConstant(*Value.children[0]) && IsNumericConstant(*Value.children[1]);
  return false;
}

bool ExpandForwardPack(std::unique_ptr<lex::Node> &Node, std::string_view PackName,
                       const std::vector<const lex::Node *> &Arguments,
                       const std::vector<bool> &Temporary,
                       std::unordered_set<const lex::Node *> &TemporaryNodes, unsigned &Spreads) {
  if (Node->kind == K::ast_name && Node->text == PackName)
    return false;
  for (auto &Child : Node->children) {
    if (Child->kind != K::ast_call)
      if (!ExpandForwardPack(Child, PackName, Arguments, Temporary, TemporaryNodes, Spreads))
        return false;
    if (Child->kind != K::ast_call)
      continue;
    std::vector<std::unique_ptr<lex::Node>> Rewritten;
    for (auto &Argument : Child->children) {
      if (Argument->kind == K::ast_spread) {
        if (Argument->children.size() != 1 || Argument->children.front()->kind != K::ast_name ||
            Argument->children.front()->text != PackName)
          return false;
        ++Spreads;
        for (std::size_t I = 0; I < Arguments.size(); ++I) {
          if (IsNumericConstant(*Arguments[I])) {
            Rewritten.push_back(lex::Clone(*Arguments[I]));
            continue;
          }
          auto Name = std::make_unique<lex::Node>(
              lex::Node{K::ast_name, Argument->Loc, "$forward" + std::to_string(I), {}, 1});
          if (Temporary[I])
            TemporaryNodes.insert(Name.get());
          Rewritten.push_back(std::move(Name));
        }
      } else {
        if (!ExpandForwardPack(Argument, PackName, Arguments, Temporary, TemporaryNodes, Spreads))
          return false;
        Rewritten.push_back(std::move(Argument));
      }
    }
    Child->children = std::move(Rewritten);
  }
  return true;
}

bool ContainsReturn(const lex::Node &Node) {
  if (Node.kind == K::ast_return)
    return true;
  return std::any_of(Node.children.begin(), Node.children.end(), [](const auto &Child) {
    return ContainsReturn(*Child);
  });
}

} // namespace

bool sema::Sema::CheckForwardConstructor(const lex::Node &Expression, const ClassInfo &Class) {
  const auto &Constructor = *Class.Constructor;
  const lex::Node *Pack = nullptr;
  const lex::Node *Parameter = nullptr;
  for (const auto &Child : Constructor.children) {
    if (Child->kind == K::ast_generic_pack) {
      if (Pack)
        return Error(Constructor, lex::DiagnosticKind::InvalidClass), false;
      Pack = Child.get();
    } else if (Child->kind == K::ast_parameter_pack) {
      if (Parameter)
        return Error(Constructor, lex::DiagnosticKind::InvalidClass), false;
      Parameter = Child.get();
    } else if (Child->kind == K::ast_generic_parameter || Child->kind == K::ast_parameter) {
      return Error(*Child, lex::DiagnosticKind::InvalidClass), false;
    }
  }
  if (!Pack || !Parameter || Parameter->children.empty() ||
      Parameter->children.back()->kind != K::ast_type ||
      Parameter->children.back()->text != Pack->text ||
      !std::any_of(Parameter->children.begin(), Parameter->children.end(), [](const auto &Child) {
        return Child->kind == K::ast_annotation && IsBuiltinAnnotation(Child->text, "forward");
      }))
    return Error(Constructor, lex::DiagnosticKind::InvalidClass), false;

  std::vector<Type> ArgumentTypes;
  std::vector<bool> Temporary;
  for (std::size_t I = 1; I < Expression.children.size(); ++I) {
    const auto &Argument = *Expression.children[I];
    const auto *Previous = Context.ConstructionContext;
    Context.ConstructionContext = &Argument;
    auto Value = CheckExpression(Argument);
    Context.ConstructionContext = Previous;
    if (!Value)
      return false;
    ArgumentTypes.push_back(*Value);
    Temporary.push_back(Value->IsClass() && IsClassTemporary(Argument));
    if (Value->IsClass())
      CheckTransferAccess(*Value, Temporary.back(), Argument);
  }

  auto Specialized = lex::Clone(Constructor);
  if (ContainsReturn(*Specialized))
    return Error(Constructor, lex::DiagnosticKind::InvalidClass), false;
  std::erase_if(Specialized->children, [](const auto &Child) {
    return Child->kind == K::ast_generic_pack || Child->kind == K::ast_parameter_pack;
  });
  for (std::size_t I = 0; I < ArgumentTypes.size(); ++I) {
    auto Name = std::make_unique<lex::Node>(lex::Node{
        K::ast_parameter, Expression.children[I + 1]->Loc, "$forward" + std::to_string(I), {}, 1});
    Types[Name.get()] = ArgumentTypes[I];
    auto Body = std::find_if(Specialized->children.begin(),
                             Specialized->children.end(),
                             [](const auto &Part) { return Part->kind == K::ast_block; });
    Specialized->children.insert(Body, std::move(Name));
  }
  unsigned Spreads = 0;
  std::vector<const lex::Node *> Arguments;
  for (std::size_t I = 1; I < Expression.children.size(); ++I)
    Arguments.push_back(Expression.children[I].get());
  for (auto &Child : Specialized->children)
    if (Child->kind == K::ast_block &&
        !ExpandForwardPack(
            Child, Parameter->text, Arguments, Temporary, ForwardTemporaries, Spreads))
      return Error(*Child, lex::DiagnosticKind::UnsupportedExpression), false;
  if (Spreads != 1)
    return Error(Constructor, lex::DiagnosticKind::InvalidClass), false;

  Types[Specialized.get()] = Type{BuiltinType::Void, {}};
  auto *Body = Specialized.get();
  InlineConstructors.emplace(&Expression, std::move(Specialized));
  auto SavedScopes = std::move(Context.Scopes);
  auto SavedReturn = Context.ReturnType;
  auto SavedModule = Context.CurrentModule;
  auto SavedClass = Context.CurrentClass;
  auto *SavedConstructor = Context.CurrentConstructor;
  auto *SavedContext = Context.ConstructionContext;
  auto *SavedTarget = Context.InitializingTarget;
  const auto SavedFields = Context.InitializedFields;
  const auto SavedFieldBase = Context.CheckingFieldBase;
  const auto SavedDestructor = Context.InDestructor;
  Context.CurrentModule = Class.Module;
  CheckClassMember(*Body, Class);
  Context.Scopes = std::move(SavedScopes);
  Context.ReturnType = SavedReturn;
  Context.CurrentModule = std::move(SavedModule);
  Context.CurrentClass = std::move(SavedClass);
  Context.CurrentConstructor = SavedConstructor;
  Context.ConstructionContext = SavedContext;
  Context.InitializingTarget = SavedTarget;
  Context.InitializedFields = SavedFields;
  Context.CheckingFieldBase = SavedFieldBase;
  Context.InDestructor = SavedDestructor;
  return true;
}

void sema::Sema::CheckClassLayouts() {
  std::unordered_map<std::string, unsigned> States;
  std::function<bool(ClassInfo &)> Visit = [&](ClassInfo &Class) {
    if (States[Class.QualifiedName] == 2)
      return true;
    if (States[Class.QualifiedName] == 1) {
      Error(*Class.Node, lex::DiagnosticKind::RecursiveClass);
      return false;
    }
    States[Class.QualifiedName] = 1;
    std::uint64_t Size = 0;
    unsigned Alignment = 1;
    unsigned LayoutIndex = 0;
    if (Class.CLayout && Class.Fields.empty()) {
      Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
      return false;
    }
    for (auto &Field : Class.Fields) {
      if (!Class.Constructor && !CanZeroInitialize(Field.Value)) {
        Error(*Field.Node, lex::DiagnosticKind::InvalidEnumInitialization);
        return false;
      }
      if (Field.Value.IsClass() && !Visit(Classes.at(Field.Value.ClassName)))
        return false;
      if (Field.Value.IsClass() && !Class.RawStorage) {
        const auto &Child = Classes.at(Field.Value.ClassName);
        if (Child.Module != Class.Module &&
            (!Child.Public || (Child.Copy && !IsPublic(*Child.Copy)) ||
             (Child.Move && !IsPublic(*Child.Move)))) {
          Error(*Field.Node, lex::DiagnosticKind::PrivateDeclaration);
          return false;
        }
      }
      if (Class.CLayout) {
        auto Element = Field.Value;
        while (Element.IsArray())
          Element = Element.Indexed();
        const bool UnsupportedCScalar =
            !Element.IsPointer() && !Element.IsClass() && !Element.IsRecord() &&
            !Element.IsEnum() && !Element.IsFunction() &&
            (Element.Element == BuiltinType::Char || GetBitWidth(Element) > 64);
        if (UnsupportedCScalar ||
            (!Element.IsPointer() && Element.IsClass() && !Classes.at(Element.ClassName).CLayout) ||
            Element.IsResults() || Element.IsVoid() || Element.IsSlice() ||
            (!Element.IsClass() && !Layout.GetAlignment(Element))) {
          Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
          return false;
        }
      }
      // A generated default constructor constructs class fields by calling
      // their own default constructor, which must exist and be reachable.
      if (Field.Value.IsClass() && !Class.Constructor) {
        const auto &Child = Classes.at(Field.Value.ClassName);
        if (!Child.DefaultConstructible) {
          Error(*Field.Node, lex::DiagnosticKind::MissingDefaultConstructor);
          return false;
        }
        const auto Constructor = Functions.find(Child.QualifiedName + ".init");
        const bool ConstructorPublic =
            Constructor != Functions.end() ? Constructor->second.Public : Child.Public;
        if (Child.Module != Class.Module && (!Child.Public || !ConstructorPublic)) {
          Error(*Field.Node, lex::DiagnosticKind::PrivateDeclaration);
          return false;
        }
      }
      if (!Field.Value.IsClass() && !Field.Value.IsRecord() && !Field.Value.IsPointer() &&
          GetBitWidth(Field.Value) > 128) {
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
        return false;
      }
      auto Element = Field.Value;
      std::uint64_t Count = 1;
      while (Element.IsArray()) {
        const auto Dimension = Element.ArrayLength();
        if (Count > std::numeric_limits<unsigned>::max() / 8 / Dimension) {
          Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
          return false;
        }
        Count *= Dimension;
        Element = Element.Indexed();
      }
      std::uint64_t FieldSize = (GetBitWidth(Element) + 7) / 8;
      unsigned FieldAlignment = GetAlignment(Element);
      if (Element.IsPointer() && Element.PointerDepth == 1 &&
          Element.Element == BuiltinType::Class) {
        const auto *Pointee = GetClass(Element.ClassName);
        if (Pointee && Pointee->IsInterface)
          FieldSize = (Pointee->InterfaceMethods.size() + 1) * Layout.GetPointerBitWidth() / 8;
      }
      if (Element.IsClass()) {
        const auto &Child = Classes.at(Element.ClassName);
        FieldSize = Child.Size;
        FieldAlignment = Child.Alignment;
      }
      if (Class.CLayout && !Element.IsClass())
        FieldAlignment = Layout.GetAlignment(Element);
      if (!FieldAlignment)
        FieldAlignment =
            std::min<std::uint64_t>(16, std::bit_ceil(std::max<std::uint64_t>(1, FieldSize)));
      Field.Alignment = FieldAlignment;
      if (FieldSize > std::numeric_limits<unsigned>::max() / 8 / Count) {
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
        return false;
      }
      FieldSize *= Count;
      if (!FieldSize) {
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
        return false;
      }
      const auto Padding = (FieldAlignment - Size % FieldAlignment) % FieldAlignment;
      if (Padding)
        ++LayoutIndex;
      Size += Padding;
      Field.Offset = Size;
      Field.LayoutIndex = LayoutIndex++;
      Size += FieldSize;
      Alignment = std::max(Alignment, FieldAlignment);
      if (Size > std::numeric_limits<unsigned>::max() / 8 - Alignment) {
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
        return false;
      }
    }
    if (Class.RawStorage && (Class.Fields.size() != 1 || Class.Fields.front().Name != "data" ||
                             !Class.BaseName.empty() || !Class.Constructor)) {
      Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
      return false;
    }
    if (!Class.RawStorage)
      for (const auto &Field : Class.Fields) {
        const auto *Stored = Field.Value.IsClass() ? GetClass(Field.Value) : nullptr;
        if (Stored && Stored->RawStorage &&
            (!Class.Constructor || !Class.Destructor || !Class.Copy || !Class.Move)) {
          Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
          return false;
        }
      }
    Class.Size = std::max<std::uint64_t>(1, (Size + Alignment - 1) / Alignment * Alignment);
    Class.Alignment = Alignment;
    const auto Id = *Reflection.GetId(*Class.Node);
    Reflection.Records[Id].BitWidth = Class.Size * 8;
    Reflection.Records[Id].Alignment = Class.Alignment;
    States[Class.QualifiedName] = 2;
    return true;
  };
  for (auto &[Name, Class] : Classes)
    Visit(Class);
  for (auto &Record : Reflection.Records) {
    if (Record.Kind != MetaKind::Type || Record.TypeKind != MetaTypeKind::Record)
      continue;
    if (const auto *Class = GetClass(Record.QualifiedName)) {
      Record.BitWidth = Class->Size * 8;
      Record.Alignment = Class->Alignment;
    }
  }
}

void sema::Sema::CheckClassMember(const lex::Node &Member, const ClassInfo &Class) {
  Context.Scopes.clear();
  Context.Scopes.emplace_back();
  Context.CurrentClass = Class.QualifiedName;
  Context.CurrentConstructor =
      Member.kind == K::ast_constructor || Class.Copy == &Member || Class.Move == &Member ? &Member
                                                                                          : nullptr;
  Context.InitializedFields = 0;
  Context.InDestructor = Member.kind == K::ast_destructor;
  Type Receiver{BuiltinType::Class, {}};
  Receiver.ClassName = Class.QualifiedName;
  Receiver.AddPointer();
  const bool Static =
      std::any_of(Member.children.begin(), Member.children.end(), [](const auto &Part) {
        return Part->kind == K::ast_annotation && IsBuiltinAnnotation(Part->text, "static");
      });
  if (!Static)
    Context.Scopes.back().emplace("this", Receiver);

  const lex::Node *Body = nullptr;
  const auto &Result = Types.at(&Member);
  Context.ReturnType = Result.IsVoid() ? std::nullopt : std::optional<Type>(Result);
  for (const auto &Child : Member.children) {
    if (Child->kind == K::ast_parameter) {
      if (!Context.Scopes.back().emplace(Child->text, Types.at(Child.get())).second)
        Error(*Child, lex::DiagnosticKind::DuplicateParameter);
    } else if (Child->kind == K::ast_block)
      Body = Child.get();
  }
  if (!Body) {
    Context.CurrentConstructor = nullptr;
    Context.CurrentClass.clear();
    Context.InDestructor = false;
    return;
  }

  if (Context.CurrentConstructor) {
    Context.Scopes.emplace_back();
    if (Class.RawStorage) {
      CheckBlock(*Body);
      Context.Scopes.pop_back();
      Context.CurrentConstructor = nullptr;
      Context.CurrentClass.clear();
      Context.InDestructor = false;
      return;
    }
    if (Body->children.size() < Class.UserFieldCount)
      Error(*Body, lex::DiagnosticKind::InvalidInitialization);
    for (std::size_t I = 0; I < Body->children.size(); ++I) {
      const auto &Statement = *Body->children[I];
      if (I == 0 && !Class.BaseName.empty()) {
        if (Statement.kind != K::ast_expr_stmt || Statement.children.size() != 1 ||
            Statement.children[0]->kind != K::ast_call || Statement.children[0]->children.empty() ||
            Statement.children[0]->children[0]->kind != K::ast_name ||
            Statement.children[0]->children[0]->text != "super") {
          Error(Statement, lex::DiagnosticKind::InvalidInitialization);
          continue;
        }
        CheckExpression(*Statement.children[0]);
        ++Context.InitializedFields;
        continue;
      }
      if (I < Class.UserFieldCount) {
        if (Statement.kind != K::ast_assign || Statement.children.size() != 2) {
          Error(Statement, lex::DiagnosticKind::InvalidInitialization);
          continue;
        }
        const auto &Target = *Statement.children[0];
        bool Matches = Target.kind == K::ast_name && Target.text == Class.Fields[I].Name &&
                       !FindName(Target.text);
        Matches |= Target.kind == K::ast_member && Target.text == Class.Fields[I].Name &&
                   Target.children.size() == 1 && Target.children[0]->kind == K::ast_name &&
                   Target.children[0]->text == "this";
        if (!Matches) {
          Error(Target, lex::DiagnosticKind::InvalidInitialization);
          continue;
        }
        Context.InitializingTarget = &Target;
        auto TargetType = CheckExpression(Target);
        Context.InitializingTarget = nullptr;
        const auto &Value = *Statement.children[1];
        Context.ConstructionContext = &Value;
        auto ValueType = CheckExpression(Value, TargetType);
        Context.ConstructionContext = nullptr;
        if (TargetType && TargetType->IsClass() && (!ValueType || *ValueType != *TargetType))
          Error(Value, lex::DiagnosticKind::ClassValueOperation);
        if (TargetType && TargetType->IsClass() && ValueType)
          CheckTransferAccess(*TargetType, IsClassTemporary(Value), Value);
        ++Context.InitializedFields;
      } else {
        CheckStatement(Statement, 0);
      }
    }
    Context.Scopes.pop_back();
  } else {
    CheckBlock(*Body);
  }
  if (Context.ReturnType && !AlwaysReturns(*Body))
    Error(*Body, lex::DiagnosticKind::MissingReturn);
  Context.CurrentConstructor = nullptr;
  Context.CurrentClass.clear();
  Context.InDestructor = false;
}

void sema::Sema::CheckTransferAccess(const Type &Value, bool Move, const lex::Node &Site) {
  if (!Value.IsClass())
    return;
  const auto *Class = GetClass(Value);
  const auto &AccessModule =
      Value.GenericArgument ? Value.GenericOriginModule : Context.CurrentModule;
  if (!Class || Class->Module == AccessModule)
    return;
  const auto *Method = Move ? Class->Move : Class->Copy;
  if (!Class->Public || (Method && !IsPublic(*Method)))
    Error(Site, lex::DiagnosticKind::PrivateDeclaration);
}

bool sema::Sema::CheckClassContracts() {
  using K = lex::NodeKind;
  std::function<bool(const ClassInfo &, std::string_view, std::string_view)> InterfaceHasMethod =
      [&](const ClassInfo &Interface, std::string_view Method, std::string_view Signature) {
        for (const auto *Candidate :
             FindOverloads(Interface.QualifiedName + "." + std::string(Method)))
          if (Candidate->Signature == Signature)
            return true;
        for (const auto &Parent : Interface.Interfaces)
          if (InterfaceHasMethod(*GetClass(Parent), Method, Signature))
            return true;
        return false;
      };

  const auto SameInterfaceSignature = [&](const std::string &Left, const std::string &Right) {
    const auto &A = Functions.at(Left);
    const auto &B = Functions.at(Right);
    if (A.Parameters.size() != B.Parameters.size() || A.Return != B.Return)
      return false;
    for (std::size_t I = 1; I < A.Parameters.size(); ++I)
      if (A.Parameters[I] != B.Parameters[I])
        return false;
    return true;
  };
  std::function<void(ClassInfo &)> CollectInterfaceMethods = [&](ClassInfo &Class) {
    if (!Class.IsInterface || !Class.InterfaceMethods.empty())
      return;
    for (const auto &ParentName : Class.Interfaces) {
      auto &Parent = Classes.at(ParentName);
      CollectInterfaceMethods(Parent);
      for (const auto &Method : Parent.InterfaceMethods) {
        const auto Duplicate = std::find_if(Class.InterfaceMethods.begin(),
                                            Class.InterfaceMethods.end(),
                                            [&](const auto &Existing) {
                                              return Functions.at(Existing).Signature ==
                                                     Functions.at(Method).Signature;
                                            });
        if (Duplicate == Class.InterfaceMethods.end())
          Class.InterfaceMethods.push_back(Method);
        else if (!SameInterfaceSignature(*Duplicate, Method))
          Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
      }
    }
    for (const auto &Member : Class.Node->children) {
      if (Member->kind != K::ast_function)
        continue;
      const auto Method = FunctionKeys.at(Member.get());
      const auto Existing = std::find_if(
          Class.InterfaceMethods.begin(), Class.InterfaceMethods.end(), [&](const auto &Candidate) {
            return Functions.at(Candidate).Signature == Functions.at(Method).Signature;
          });
      if (Existing == Class.InterfaceMethods.end())
        Class.InterfaceMethods.push_back(Method);
      else if (!SameInterfaceSignature(*Existing, Method))
        Error(*Member, lex::DiagnosticKind::InvalidClass);
    }
  };
  for (auto &[Name, Class] : Classes)
    CollectInterfaceMethods(Class);

  for (auto &[Name, Class] : Classes) {
    if (Class.IsInterface)
      continue;
    for (const auto &Member : Class.Node->children) {
      if (Member->kind != K::ast_function || Member->text == "copy" || Member->text == "move")
        continue;
      const auto &Method = *FindFunction(*Member);
      const ClassInfo *SlotOwner = nullptr;
      const FunctionInfo *Inherited = nullptr;
      for (auto BaseName = Class.BaseName; !BaseName.empty();) {
        const auto *Base = GetClass(BaseName);
        if (!Inherited) {
          for (const auto *Candidate : FindOverloads(BaseName + "." + Member->text))
            if (Candidate->Signature == Method.Signature) {
              Inherited = Candidate;
              break;
            }
        }
        if (Base->VirtualSlots.contains(Method.Signature))
          SlotOwner = Base;
        BaseName = Base->BaseName;
      }
      if (Method.Static) {
        if (Method.Virtual || Method.Override || Inherited)
          Error(*Member, lex::DiagnosticKind::InvalidClass);
      } else if (Method.Override) {
        bool InterfaceMethod = false;
        for (const auto &Interface : Class.Interfaces)
          InterfaceMethod |=
              InterfaceHasMethod(*GetClass(Interface), Member->text, Method.Signature);
        if ((!SlotOwner && !InterfaceMethod) || Method.Virtual || (SlotOwner && !Inherited) ||
            (Inherited &&
             (Method.Parameters.size() != Inherited->Parameters.size() ||
              Method.Return != Inherited->Return || (Inherited->Public && !Method.Public)))) {
          Error(*Member, lex::DiagnosticKind::InvalidClass);
          continue;
        }
        bool Matching = true;
        if (Inherited)
          for (std::size_t I = 1; I < Method.Parameters.size(); ++I)
            Matching &= Method.Parameters[I] == Inherited->Parameters[I];
        if (!Matching)
          Error(*Member, lex::DiagnosticKind::TypeMismatch);
        if (SlotOwner)
          Class.OverrideSlots.emplace(Method.Signature,
                                      std::make_pair(SlotOwner->QualifiedName,
                                                     SlotOwner->VirtualSlots.at(Method.Signature)));
      } else if (Inherited) {
        Error(*Member, lex::DiagnosticKind::InvalidClass);
      }
    }
    if (!Class.BaseName.empty() && (Class.Copy || Class.Move))
      Error(*Class.Node, lex::DiagnosticKind::InvalidClass);

    std::function<void(const ClassInfo &)> CheckInterface = [&](const ClassInfo &Interface) {
      for (const auto &Member : Interface.Node->children) {
        if (Member->kind != K::ast_function)
          continue;
        const auto &Required = *FindFunction(*Member);
        const FunctionInfo *Implementation = nullptr;
        for (auto OwnerName = Class.QualifiedName; !OwnerName.empty();) {
          for (const auto *Candidate : FindOverloads(OwnerName + "." + Member->text))
            if (Candidate->Signature == Required.Signature) {
              Implementation = Candidate;
              break;
            }
          if (Implementation)
            break;
          OwnerName = GetClass(OwnerName)->BaseName;
        }
        if (!Implementation || Implementation->Parameters.size() != Required.Parameters.size() ||
            Implementation->Return != Required.Return ||
            (Required.Public && !Implementation->Public)) {
          Error(*Class.Node, lex::DiagnosticKind::InvalidClass);
          continue;
        }
        for (std::size_t I = 1; I < Required.Parameters.size(); ++I)
          if (Implementation->Parameters[I] != Required.Parameters[I])
            Error(*Class.Node, lex::DiagnosticKind::TypeMismatch);
      }
      for (const auto &Parent : Interface.Interfaces)
        CheckInterface(*GetClass(Parent));
    };
    for (const auto &Interface : Class.Interfaces)
      CheckInterface(*GetClass(Interface));
  }

  CheckClassLayouts();
  if (!Diagnostics.empty())
    return false;

  return Diagnostics.empty();
}
