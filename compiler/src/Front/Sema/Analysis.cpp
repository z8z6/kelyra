#include "Front/Sema/Analysis.h"

#include "Front/AST/AST.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <vector>

using namespace kelyra;

namespace {
llvm::cl::opt<bool> DumpClassLayout{
    "dump-class-layout",
    llvm::cl::desc("Dump class field offsets, sizes, and alignment"),
    llvm::cl::cat(Option::KelyraCategory)};

std::string TypeName(const sema::Type &Type) {
  if (Type.IsPointer())
    return "*" + TypeName(Type.Pointee());
  if (Type.IsArray())
    return "[" + std::to_string(Type.ArrayLength()) + "]" + TypeName(Type.Indexed());
  if (Type.IsFunction())
    return "fn";
  if (Type.Element == sema::BuiltinType::Class)
    return Type.ClassName;
  if (!Type.CName.empty())
    return "c." + Type.CName;
  return std::string(sema::GetBuiltinTypeInfo(Type.Element).Name);
}

std::uint64_t FieldSize(const sema::Sema &Analysis, sema::Type Type) {
  std::uint64_t Count = 1;
  while (Type.IsArray()) {
    Count *= Type.ArrayLength();
    Type = Type.Indexed();
  }
  if (Type.IsClass())
    return Count * Analysis.GetClass(Type)->Size;
  if (Type.IsPointer() && Type.PointerDepth == 1 && Type.Element == sema::BuiltinType::Class) {
    const auto *Class = Analysis.GetClass(Type.ClassName);
    if (Class && Class->IsInterface)
      return Count * (Class->InterfaceMethods.size() + 1) *
             (Analysis.GetTargetLayout().GetPointerBitWidth() / 8);
  }
  return Count * (Analysis.GetBitWidth(Type) + 7) / 8;
}

} // namespace

static void DumpClassLayouts(const sema::Sema &Analysis) {
  std::vector<const sema::ClassInfo *> Classes;
  for (const auto &[Name, Class] : Analysis.GetClasses())
    if (!Class.IsInterface)
      Classes.push_back(&Class);
  std::sort(Classes.begin(), Classes.end(), [](const auto *Left, const auto *Right) {
    return Left->QualifiedName < Right->QualifiedName;
  });
  for (const auto *Class : Classes) {
    std::cout << "class " << Class->QualifiedName << " size=" << Class->Size
              << " align=" << Class->Alignment << '\n';
    std::function<void(const sema::ClassInfo &, std::uint64_t, unsigned)> PrintFields =
        [&](const sema::ClassInfo &Owner, std::uint64_t BaseOffset, unsigned Depth) {
          for (const auto &Field : Owner.Fields) {
            std::cout << std::string(Depth * 2 + 2, ' ') << '+' << BaseOffset + Field.Offset
                      << " size=" << FieldSize(Analysis, Field.Value)
                      << " align=" << Field.Alignment << " index=" << Field.LayoutIndex << ' '
                      << Field.Name << ": " << TypeName(Field.Value) << '\n';
            if (Field.Name == "$base")
              PrintFields(*Analysis.GetClass(Field.Value), BaseOffset + Field.Offset, Depth + 1);
          }
        };
    PrintFields(*Class, 0, 0);
  }
}

bool sema::Analyze(const ModuleLoader &Loader, const cimport::ImportResult &Declarations,
                   sema::Sema &Analysis, bool RequireEntrypoint) {
  if (!Analysis.GetTargetLayout().IsValid()) {
    kerr() << "unsupported target: " << Option::Target.getValue() << '\n';
    return false;
  }
  const auto &Modules = Loader.GetModules();
  std::vector<sema::ModuleInput> Inputs;
  for (const auto *Module : Modules)
    Inputs.push_back({Module->Lex.root.get(), Module->IsEntry, Module->IsExternal, Module->Name});
  const bool Valid = Analysis.CheckModules(Inputs, Declarations) &&
                     (!RequireEntrypoint || Analysis.CheckEntrypoint(*Loader.GetEntry().Lex.root));
  for (const auto &Warning : Analysis.GetWarnings())
    kwarn() << Warning.Loc.File << ':' << Warning.Loc.Line << ':' << Warning.Loc.Column
            << ": warning: " << Warning.Message << '\n';
  if (!Valid)
    for (const auto &Diagnostic : Analysis.GetDiagnostics()) {
      std::ostringstream Message;
      Message << Diagnostic;
      kerr() << Message.str() << '\n';
    }
  if (Valid && DumpClassLayout)
    DumpClassLayouts(Analysis);
  return Valid;
}
