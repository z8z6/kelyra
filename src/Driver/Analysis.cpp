#include "Command.h"

#include "Lexer/Lexer.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <vector>

using namespace kelyra;

void driver::DumpAstIfRequested(const ModuleLoader &Loader) {
  if (Option::LexDumpAst)
    std::cout << lex::Lexer().dumpAst(*Loader.GetModules().front().Parsed.root)
              << '\n';
}

bool driver::WantsCompilation() {
  return Option::LexVerify || Option::DumpClassLayout || Option::EmitMlir ||
         Option::EmitObject || Option::EmitExecutable;
}

namespace {
std::string TypeName(const sema::Type &Type) {
  if (Type.IsPointer())
    return "*" + TypeName(Type.Pointee());
  if (Type.IsArray())
    return "[" + std::to_string(Type.ArrayLength()) + "]" +
           TypeName(Type.Indexed());
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
  if (Type.IsPointer() && Type.PointerDepth == 1 &&
      Type.Element == sema::BuiltinType::Class) {
    const auto *Class = Analysis.GetClass(Type.ClassName);
    if (Class && Class->IsInterface)
      return Count * (Class->InterfaceMethods.size() + 1) * sizeof(void *);
  }
  return Count * (sema::GetBitWidth(Type) + 7) / 8;
}

} // namespace

void driver::DumpClassLayouts(const sema::Sema &Analysis) {
  std::vector<const sema::ClassInfo *> Classes;
  for (const auto &[Name, Class] : Analysis.GetClasses())
    if (!Class.IsInterface)
      Classes.push_back(&Class);
  std::sort(Classes.begin(), Classes.end(),
            [](const auto *Left, const auto *Right) {
              return Left->QualifiedName < Right->QualifiedName;
            });
  for (const auto *Class : Classes) {
    std::cout << "class " << Class->QualifiedName << " size=" << Class->Size
              << " align=" << Class->Alignment << '\n';
    std::function<void(const sema::ClassInfo &, std::uint64_t, unsigned)>
        PrintFields = [&](const sema::ClassInfo &Owner,
                          std::uint64_t BaseOffset, unsigned Depth) {
          for (const auto &Field : Owner.Fields) {
            std::cout << std::string(Depth * 2 + 2, ' ') << '+'
                      << BaseOffset + Field.Offset
                      << " size=" << FieldSize(Analysis, Field.Value)
                      << " align=" << Field.Alignment
                      << " index=" << Field.LayoutIndex << ' ' << Field.Name
                      << ": " << TypeName(Field.Value) << '\n';
            if (Field.Name == "$base")
              PrintFields(*Analysis.GetClass(Field.Value),
                          BaseOffset + Field.Offset, Depth + 1);
          }
        };
    PrintFields(*Class, 0, 0);
  }
}

bool driver::Analyze(const ModuleLoader &Loader,
                     const cimport::ImportResult &Declarations,
                     sema::Sema &Analysis) {
  const auto &Modules = Loader.GetModules();
  std::vector<sema::ModuleInput> Inputs;
  for (const auto &Module : Modules)
    Inputs.push_back(
        {Module.Parsed.root.get(), Module.IsEntry, Module.IsExternal});
  if (Option::Progress)
    kinfo() << "  [check] " << Modules.size() << " Kelyra module(s)\n";
  const bool Valid =
      Analysis.CheckModules(Inputs, Declarations.Functions,
                            Declarations.Types) &&
      (!(Option::EmitExecutable ||
         (Option::EmitObject && Option::Runtime == "freestanding")) ||
       Analysis.CheckEntrypoint(*Modules.front().Parsed.root));
  for (const auto &Warning : Analysis.GetWarnings())
    kwarn() << Warning.Loc.File << ':' << Warning.Loc.Line << ':'
            << Warning.Loc.Column << ": warning: " << Warning.Message << '\n';
  if (!Valid)
    for (const auto &Diagnostic : Analysis.GetDiagnostics()) {
      std::ostringstream Message;
      Message << Diagnostic;
      kerr() << Message.str() << '\n';
    }
  return Valid;
}
