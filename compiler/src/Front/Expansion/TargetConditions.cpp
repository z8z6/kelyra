#include "Front/Expansion/TargetConditions.h"
#include "Support/BuiltinAnnotation.h"
#include "Support/Log.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <optional>
#include <string_view>

using namespace kelyra;

namespace {
std::string CfgEnumMember(const lex::Node &Value) {
  using K = lex::NodeKind;
  if (Value.kind == K::ast_name)
    return Value.text;
  if (Value.kind != K::ast_member || Value.children.size() != 1)
    return {};
  auto Owner = CfgEnumMember(*Value.children.front());
  return Owner.empty() ? std::string{} : Owner + "." + Value.text;
}

std::optional<std::string_view> CfgTargetValue(std::string_view Owner, std::string_view Variant) {
  if (Variant == "Any")
    return "";
  if (Owner == "os") {
    if (Variant == "Windows")
      return "windows";
    if (Variant == "Linux")
      return "linux";
    if (Variant == "MacOS")
      return "macos";
  } else if (Owner == "arch") {
    if (Variant == "X86_64")
      return "x86_64";
    if (Variant == "AArch64")
      return "aarch64";
  }
  return std::nullopt;
}

} // namespace

bool kelyra::ApplyTargetConditions(lex::Node &Root, const std::string &Path,
                                   const std::string &TargetTriple, bool &ModuleEnabled) {
  using K = lex::NodeKind;
  const llvm::Triple Target(TargetTriple);
  const std::string OS = Target.isOSWindows() ? "windows"
                         : Target.isOSLinux() ? "linux"
                         : Target.isMacOSX()  ? "macos"
                                              : Target.getOSName().str();
  const std::string Arch = Target.getArchName().str();
  bool Valid = true;
  auto &Declarations = Root.children;
  Declarations.erase(
      std::remove_if(Declarations.begin(),
                     Declarations.end(),
                     [&](std::unique_ptr<lex::Node> &Declaration) {
                       bool Enabled = true;
                       auto &Parts = Declaration->children;
                       Parts.erase(
                           std::remove_if(
                               Parts.begin(),
                               Parts.end(),
                               [&](const std::unique_ptr<lex::Node> &Part) {
                                 if (Part->kind != K::ast_annotation ||
                                     !IsBuiltinAnnotation(Part->text, "cfg"))
                                   return false;
                                 if (Part->children.empty()) {
                                   Valid = false;
                                   return true;
                                 }
                                 bool SeenOS = false;
                                 bool SeenArch = false;
                                 bool Constrained = false;
                                 for (const auto &Child : Part->children) {
                                   const auto &Argument = *Child;
                                   if (Argument.kind != K::ast_annotation_argument ||
                                       Argument.children.size() != 1) {
                                     Valid = false;
                                     continue;
                                   }
                                   const auto &Value = *Argument.children.front();
                                   auto Member = CfgEnumMember(Value);
                                   if (Member.starts_with("std.annotation."))
                                     Member.erase(0, sizeof("std.annotation.") - 1);
                                   const auto Separator = Member.rfind('.');
                                   const auto Owner = Separator == std::string::npos
                                                          ? std::string{}
                                                          : Member.substr(0, Separator);
                                   const auto Variant = Separator == std::string::npos
                                                            ? std::string{}
                                                            : Member.substr(Separator + 1);
                                   const bool IsOS = Argument.text == "os" ||
                                                     (Argument.text.empty() && Owner == "os");
                                   const bool IsArch = Argument.text == "arch" ||
                                                       (Argument.text.empty() && Owner == "arch");
                                   if ((!IsOS && !IsArch) || (IsOS && SeenOS) ||
                                       (IsArch && SeenArch) || Owner != (IsOS ? "os" : "arch")) {
                                     Valid = false;
                                     continue;
                                   }
                                   SeenOS |= IsOS;
                                   SeenArch |= IsArch;
                                   const auto Wanted = CfgTargetValue(Owner, Variant);
                                   if (!Wanted) {
                                     Valid = false;
                                     continue;
                                   }
                                   if (!Wanted->empty()) {
                                     Constrained = true;
                                     Enabled &= *Wanted == (IsOS ? OS : Arch);
                                   }
                                 }
                                 if (!Constrained)
                                   Valid = false;
                                 return true;
                               }),
                           Parts.end());
                       if (Declaration->kind == K::ast_import &&
                           std::any_of(Parts.begin(), Parts.end(), [](const auto &Part) {
                             return Part->kind == K::ast_annotation;
                           }))
                         Valid = false;
                       if (Declaration->kind == K::ast_module_decl) {
                         ModuleEnabled = Enabled;
                         return false;
                       }
                       return !Enabled;
                     }),
      Declarations.end());
  if (!Valid)
    kerr() << Path << ": error: invalid @cfg annotation\n";
  return Valid;
}
