#include "Driver/Command.h"
#include "Support/Option.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

int main(int argc, char **argv) {
  llvm::cl::HideUnrelatedOptions(kelyra::Option::KelyraCategory);
  if (!llvm::cl::ParseCommandLineOptions(argc, argv, "Kelyra compiler\n",
                                         &llvm::errs())) {
    llvm::cl::PrintHelpMessage(true, true);
    return 2;
  }
  return kelyra::driver::Run();
}
