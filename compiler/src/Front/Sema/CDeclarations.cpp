#include "Front/Sema/CDeclarations.h"
#include "Support/Log.h"
#include <algorithm>

using namespace kelyra;

bool sema::CDeclarations::Merge(const CDeclarations &Other) {
  const auto MergeNamed = [](auto &Destination, const auto &Source) {
    for (const auto &Value : Source) {
      const auto Existing = std::find_if(Destination.begin(),
                                         Destination.end(),
                                         [&](const auto &Item) { return Item.Name == Value.Name; });
      if (Existing == Destination.end())
        Destination.push_back(Value);
      else if (*Existing != Value) {
        kerr() << "conflicting C declaration: " << Value.Name << '\n';
        return false;
      }
    }
    return true;
  };
  if (!MergeNamed(Functions, Other.Functions) || !MergeNamed(Types, Other.Types) ||
      !MergeNamed(Constants, Other.Constants))
    return false;
  for (const auto &[Name, Source] : Other.Headers) {
    const auto [It, Inserted] = Headers.emplace(Name, Source);
    if (!Inserted && It->second != Source) {
      kerr() << "conflicting C header snapshot: " << Name << '\n';
      return false;
    }
  }
  return true;
}
