#include "CImport/CImporter.h"
#include "CodeGen/IRGen.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"
#include "Support/Option.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace llvm;
using namespace kelyra;

namespace {
struct SourceModule {
  std::string Path;
  lex::ParseResult Parsed;
  bool IsEntry = false;
};

class ModuleLoader {
  enum class State { Loading, Loaded };

  std::filesystem::path Root;
  lex::Lexer Lexer;
  std::deque<SourceModule> Modules;
  std::unordered_map<std::string, State> States;
  std::vector<std::string> CHeaders;

  bool Load(const std::filesystem::path &Path, std::string Expected,
            bool IsEntry) {
    std::ifstream Input(Path, std::ios::binary);
    if (!Input) {
      std::cerr << "cannot open source file: " << Path.string() << '\n';
      return false;
    }
    std::string Source((std::istreambuf_iterator<char>(Input)), {});
    if (Input.bad()) {
      std::cerr << "cannot read source file: " << Path.string() << '\n';
      return false;
    }

    Modules.emplace_back();
    auto &Module = Modules.back();
    Module.Path = Path.string();
    Module.IsEntry = IsEntry;
    Module.Parsed = Lexer.parse(std::move(Source), Module.Path);
    for (const auto &Diagnostic : Module.Parsed.diagnostics)
      std::cerr << Diagnostic << '\n';
    if (!Module.Parsed.ok())
      return false;

    std::string Name;
    for (const auto &Child : Module.Parsed.root->children)
      if (Child->kind == lex::TokenKind::ast_module_decl)
        Name = Child->text;
    if (!Expected.empty() && Name != Expected) {
      std::cerr << Module.Path << ": error: expected module '" << Expected
                << "'\n";
      return false;
    }
    if (Name.empty() && !IsEntry) {
      std::cerr << Module.Path << ": error: imported file has no module "
                << "declaration\n";
      return false;
    }
    if (!Name.empty()) {
      const auto [It, Inserted] = States.emplace(Name, State::Loading);
      if (!Inserted) {
        std::cerr << Module.Path << ": error: cyclic or duplicate module '"
                  << Name << "'\n";
        return false;
      }
    }

    for (const auto &Child : Module.Parsed.root->children) {
      if (Child->kind != lex::TokenKind::ast_import)
        continue;
      if (Child->text == "c") {
        if (Child->children.size() != 1) {
          std::cerr << Module.Path
                    << ": error: 'import c' requires a header string\n";
          return false;
        }
        CHeaders.push_back(
            std::filesystem::absolute(Path.parent_path() /
                                      Child->children.front()->text)
                .lexically_normal()
                .string());
        continue;
      }
      const auto Known = States.find(Child->text);
      if (Known != States.end()) {
        if (Known->second == State::Loading) {
          std::cerr << Module.Path << ": error: cyclic module import '"
                    << Child->text << "'\n";
          return false;
        }
        continue;
      }
      auto Relative = Child->text;
      std::replace(Relative.begin(), Relative.end(), '.', '/');
      if (!Load(Root / (Relative + ".kly"), Child->text, false))
        return false;
    }
    if (!Name.empty())
      States[Name] = State::Loaded;
    return true;
  }

public:
  bool LoadEntry(const std::string &Path) {
    const auto Entry = std::filesystem::absolute(Path).lexically_normal();
    Root = Entry.parent_path();
    return Load(Entry, {}, true);
  }

  const std::deque<SourceModule> &GetModules() const { return Modules; }
  const std::vector<std::string> &GetCHeaders() const { return CHeaders; }
};
} // namespace

int main(int argc, char **argv) {
  cl::HideUnrelatedOptions(Option::KelyraCategory);
  if (!cl::ParseCommandLineOptions(argc, argv, "Kelyra compiler\n", &errs())) {
    errs() << "usage: kelyra [--dump-ast|--check|--emit-mlir|--emit-obj|"
              "--emit-exe] [-O0|-O1|-O2|-O3] [--safe-level=<n>] "
              "[--c-source=<file>] [--c-arg=<arg>] "
              "[-o <file>] <file>\n";
    return 2;
  }
  if (Option::OptLevel > 3) {
    errs() << "error: optimization level must be between 0 and 3\n";
    return 2;
  }
  const unsigned Actions =
      Option::LexDumpAst.getValue() + Option::LexVerify.getValue() +
      Option::EmitMlir.getValue() + Option::EmitObject.getValue() +
      Option::EmitExecutable.getValue();
  if (Option::InputFile.getValue().empty() && Actions != 0) {
    errs() << "usage: kelyra [--dump-ast|--check|--emit-mlir|--emit-obj|"
              "--emit-exe] [-O0|-O1|-O2|-O3] [--safe-level=<n>] "
              "[--c-source=<file>] [--c-arg=<arg>] "
              "[-o <file>] <file>\n";
    return 2;
  }
  if (!Option::InputFile.getValue().empty()) {
    const std::string &filename = Option::InputFile.getValue();
    if (Actions != 1) {
      std::cerr << "usage: kelyra [--dump-ast|--check|--emit-mlir|--emit-obj|"
                   "--emit-exe] [-O0|-O1|-O2|-O3] [--safe-level=<n>] "
                   "[--c-source=<file>] [--c-arg=<arg>] "
                   "[-o <file>] <file>\n";
      return 2;
    }
    if ((Option::EmitObject || Option::EmitExecutable) &&
        Option::OutputFile.getValue().empty()) {
      std::cerr << filename << ": error: output action requires -o <file>\n";
      return 2;
    }
    ModuleLoader Loader;
    if (!Loader.LoadEntry(filename))
      return 1;
    const auto &Modules = Loader.GetModules();
    const auto &Entry = Modules.front();
    std::vector<std::string> CArguments(Option::CArguments.begin(),
                                        Option::CArguments.end());
    auto CDeclarations =
        cimport::ImportHeaders(Loader.GetCHeaders(), CArguments);
    if (!CDeclarations.Ok()) {
      for (const auto &Diagnostic : CDeclarations.Diagnostics)
        std::cerr << filename << ": error: " << Diagnostic << '\n';
      return 1;
    }
    if (Option::LexDumpAst)
      std::cout << lex::Lexer().dumpAst(*Entry.Parsed.root) << '\n';
    if (Option::EmitMlir || Option::EmitObject || Option::EmitExecutable) {
      sema::Sema analysis;
      std::vector<sema::ModuleInput> Inputs;
      std::vector<const lex::Node *> Asts;
      for (const auto &Module : Modules) {
        Inputs.push_back({Module.Parsed.root.get(), Module.IsEntry});
        Asts.push_back(Module.Parsed.root.get());
      }
      const bool Valid = analysis.CheckModules(Inputs, CDeclarations.Functions,
                                               CDeclarations.Types) &&
                         (!Option::EmitExecutable ||
                          analysis.CheckEntrypoint(*Entry.Parsed.root));
      if (!Valid) {
        for (const auto &diagnostic : analysis.GetDiagnostics())
          std::cerr << diagnostic << '\n';
        return 1;
      }
      mlir::MLIRContext context;
      codegen::IRGen generator(context, analysis, Option::SafeLevel);
      auto module = generator.Generate(Asts);
      std::string CWrapperSource;
      for (const auto &Wrapper : analysis.GetCWrappers())
        CWrapperSource += Wrapper.Source;
      if (mlir::failed(mlir::verify(*module)))
        return 1;
      if (Option::EmitMlir) {
        module->print(outs());
        outs() << '\n';
      } else if (auto Error =
                     Option::EmitObject
                         ? codegen::EmitObject(*module, Option::OutputFile,
                                               Option::OptLevel, CWrapperSource,
                                               Option::CArguments)
                         : codegen::EmitExecutable(
                               *module, Option::OutputFile, Option::OptLevel,
                               Option::CSources, Option::CArguments,
                               CWrapperSource)) {
        errs() << filename << ": error: " << toString(std::move(Error)) << '\n';
        return 1;
      }
    }
    return 0;
  }

  return 0;
}
