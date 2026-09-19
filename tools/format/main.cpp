#include "Lexer/Formatter.h"
#include "Lexer/Lexer.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
void Usage(std::ostream &Output) {
  Output << "usage: kelyra-format [-i|--check] <file>...\n"
            "  -i       format files in place\n"
            "  --check  fail if any file needs formatting\n";
}
} // namespace

int main(int Argc, char **Argv) {
  bool InPlace = false;
  bool Check = false;
  std::vector<std::string> Files;
  for (int I = 1; I < Argc; ++I) {
    const std::string Argument = Argv[I];
    if (Argument == "-h" || Argument == "--help") {
      Usage(std::cout);
      return 0;
    }
    if (Argument == "-i")
      InPlace = true;
    else if (Argument == "--check")
      Check = true;
    else if (!Argument.empty() && Argument.front() == '-') {
      std::cerr << "unknown option: " << Argument << '\n';
      Usage(std::cerr);
      return 2;
    } else
      Files.push_back(Argument);
  }
  if (Files.empty() || (InPlace && Check) ||
      (!InPlace && !Check && Files.size() != 1)) {
    Usage(std::cerr);
    return 2;
  }

  bool Changed = false;
  for (const auto &Path : Files) {
    std::ifstream Input(Path, std::ios::binary);
    if (!Input) {
      std::cerr << "cannot open source file: " << Path << '\n';
      return 1;
    }
    std::string Source((std::istreambuf_iterator<char>(Input)), {});
    if (Input.bad()) {
      std::cerr << "cannot read source file: " << Path << '\n';
      return 1;
    }

    auto Parsed = kelyra::lex::Lexer().parse(Source, Path);
    if (!Parsed.ok()) {
      for (const auto &Diagnostic : Parsed.diagnostics)
        std::cerr << Diagnostic << '\n';
      return 1;
    }
    const auto Formatted = kelyra::lex::Format(Parsed);
    if (Formatted == Source)
      continue;
    Changed = true;
    if (Check) {
      std::cerr << Path << ": needs formatting\n";
      continue;
    }
    if (!InPlace) {
      std::cout << Formatted;
      continue;
    }
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    if (!Output || !(Output << Formatted)) {
      std::cerr << "cannot write source file: " << Path << '\n';
      return 1;
    }
  }
  return Check && Changed ? 1 : 0;
}
