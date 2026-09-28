#pragma once

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace kelyra::test {
inline std::string ReadSource(std::string_view RelativePath) {
  std::ifstream Input(std::string(KELYRA_TEST_ROOT) + "/" +
                      std::string(RelativePath));
  return {std::istreambuf_iterator<char>(Input), {}};
}
} // namespace kelyra::test
