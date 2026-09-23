#include "Sema/Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>

using namespace kelyra;
using K = lex::TokenKind;

namespace {
unsigned CFieldAlignment(const sema::Type &Type) {
  using T = sema::BuiltinType;
  if (Type.IsPointer() || Type.IsFunction())
    return alignof(void *);
  if (Type.IsRecord())
    return Type.Alignment;
  switch (Type.Element) {
  case T::I8:
  case T::U8:
  case T::CChar:
  case T::CSChar:
  case T::CUChar:
    return alignof(char);
  case T::I16:
  case T::U16:
  case T::CShort:
    return alignof(short);
  case T::I32:
  case T::U32:
  case T::CInt:
  case T::CUInt:
    return alignof(int);
  case T::I64:
  case T::U64:
  case T::CLongLong:
    return alignof(long long);
  case T::CLong:
    return alignof(long);
  case T::ISize:
  case T::CPtrdiff:
    return alignof(std::ptrdiff_t);
  case T::USize:
  case T::CSize:
    return alignof(std::size_t);
  case T::F32:
  case T::CFloat:
    return alignof(float);
  case T::F64:
  case T::CDouble:
    return alignof(double);
  case T::Bool:
  case T::CBool:
    return alignof(bool);
  case T::CWChar:
    return alignof(wchar_t);
  default:
    return 0;
  }
}
} // namespace

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
      if (Field.Value.IsClass() && !Visit(Classes.at(Field.Value.ClassName)))
        return false;
      if (Field.Value.IsClass()) {
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
        if ((!Element.IsPointer() && Element.IsClass() &&
             !Classes.at(Element.ClassName).CLayout) ||
            Element.IsFunction() || Element.IsResults() || Element.IsVoid() ||
            (!Element.IsClass() && !CFieldAlignment(Element))) {
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
        const bool ConstructorPublic = Constructor != Functions.end()
                                           ? Constructor->second.Public
                                           : Child.Public;
        if (Child.Module != Class.Module &&
            (!Child.Public || !ConstructorPublic)) {
          Error(*Field.Node, lex::DiagnosticKind::PrivateDeclaration);
          return false;
        }
      }
      if (!Field.Value.IsClass() && !Field.Value.IsRecord() &&
          !Field.Value.IsPointer() && GetBitWidth(Field.Value) > 128) {
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
      if (Element.IsClass()) {
        const auto &Child = Classes.at(Element.ClassName);
        FieldSize = Child.Size;
        FieldAlignment = Child.Alignment;
      }
      if (Class.CLayout && !Element.IsClass())
        FieldAlignment = CFieldAlignment(Element);
      if (!FieldAlignment)
        FieldAlignment = std::min<std::uint64_t>(
            16, std::bit_ceil(std::max<std::uint64_t>(1, FieldSize)));
      if (FieldSize > std::numeric_limits<unsigned>::max() / 8 / Count) {
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
        return false;
      }
      FieldSize *= Count;
      if (!FieldSize) {
        Error(*Field.Node, lex::DiagnosticKind::UnsupportedType);
        return false;
      }
      const auto Padding =
          (FieldAlignment - Size % FieldAlignment) % FieldAlignment;
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
    Class.Size = std::max<std::uint64_t>(1, (Size + Alignment - 1) / Alignment *
                                                Alignment);
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
    if (Record.Kind != MetaKind::Type ||
        Record.TypeKind != MetaTypeKind::Record)
      continue;
    if (const auto *Class = GetClass(Record.QualifiedName)) {
      Record.BitWidth = Class->Size * 8;
      Record.Alignment = Class->Alignment;
    }
  }
}

void sema::Sema::CheckClassMember(const lex::Node &Member,
                                  const ClassInfo &Class) {
  Scopes.clear();
  Scopes.emplace_back();
  CurrentClass = Class.QualifiedName;
  CurrentConstructor = Member.kind == K::ast_constructor ||
                               Class.Copy == &Member || Class.Move == &Member
                           ? &Member
                           : nullptr;
  InitializedFields = 0;
  InDestructor = Member.kind == K::ast_destructor;
  Type Receiver{BuiltinType::Class, {}};
  Receiver.ClassName = Class.QualifiedName;
  Receiver.AddPointer();
  Scopes.back().emplace("this", Receiver);

  const lex::Node *Body = nullptr;
  const auto &Result = Types.at(&Member);
  ReturnType = Result.IsVoid() ? std::nullopt : std::optional<Type>(Result);
  for (const auto &Child : Member.children) {
    if (Child->kind == K::ast_parameter) {
      if (!Scopes.back().emplace(Child->text, Types.at(Child.get())).second)
        Error(*Child, lex::DiagnosticKind::DuplicateParameter);
    } else if (Child->kind == K::ast_block)
      Body = Child.get();
  }
  if (!Body) {
    CurrentConstructor = nullptr;
    CurrentClass.clear();
    InDestructor = false;
    return;
  }

  if (CurrentConstructor) {
    Scopes.emplace_back();
    if (Body->children.size() < Class.Fields.size())
      Error(*Body, lex::DiagnosticKind::InvalidInitialization);
    for (std::size_t I = 0; I < Body->children.size(); ++I) {
      const auto &Statement = *Body->children[I];
      if (I < Class.Fields.size()) {
        if (Statement.kind != K::ast_assign || Statement.children.size() != 2) {
          Error(Statement, lex::DiagnosticKind::InvalidInitialization);
          continue;
        }
        const auto &Target = *Statement.children[0];
        bool Matches = Target.kind == K::ast_name &&
                       Target.text == Class.Fields[I].Name &&
                       !FindName(Target.text);
        Matches |= Target.kind == K::ast_member &&
                   Target.text == Class.Fields[I].Name &&
                   Target.children.size() == 1 &&
                   Target.children[0]->kind == K::ast_name &&
                   Target.children[0]->text == "this";
        if (!Matches) {
          Error(Target, lex::DiagnosticKind::InvalidInitialization);
          continue;
        }
        InitializingTarget = &Target;
        auto TargetType = CheckExpression(Target);
        InitializingTarget = nullptr;
        const auto &Value = *Statement.children[1];
        ConstructionContext = &Value;
        auto ValueType = CheckExpression(Value, TargetType);
        ConstructionContext = nullptr;
        if (TargetType && TargetType->IsClass() &&
            (!ValueType || *ValueType != *TargetType))
          Error(Value, lex::DiagnosticKind::ClassValueOperation);
        if (TargetType && TargetType->IsClass() && ValueType)
          CheckTransferAccess(*TargetType, IsClassTemporary(Value), Value);
        ++InitializedFields;
      } else {
        CheckStatement(Statement, 0);
      }
    }
    Scopes.pop_back();
  } else {
    CheckBlock(*Body);
  }
  if (ReturnType && !AlwaysReturns(*Body))
    Error(*Body, lex::DiagnosticKind::MissingReturn);
  CurrentConstructor = nullptr;
  CurrentClass.clear();
  InDestructor = false;
}

void sema::Sema::CheckTransferAccess(const Type &Value, bool Move,
                                     const lex::Node &Site) {
  if (!Value.IsClass())
    return;
  const auto *Class = GetClass(Value);
  const auto &AccessModule =
      Value.GenericArgument ? Value.GenericOriginModule : CurrentModule;
  if (!Class || Class->Module == AccessModule)
    return;
  const auto *Method = Move ? Class->Move : Class->Copy;
  if (!Class->Public || (Method && !IsPublic(*Method)))
    Error(Site, lex::DiagnosticKind::PrivateDeclaration);
}
