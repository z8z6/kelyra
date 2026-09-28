#pragma once

#include <string_view>

namespace kelyra {
inline constexpr std::string_view BuiltinAnnotationModule = "std.annotation";

inline constexpr bool IsBuiltinAnnotation(std::string_view Actual,
                                          std::string_view Name) {
  constexpr std::string_view Prefix = "std.annotation.";
  return Actual == Name ||
         (Actual.starts_with(Prefix) && Actual.substr(Prefix.size()) == Name);
}
} // namespace kelyra
