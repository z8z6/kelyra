//
// Created by zhou_zhengming on 2026/10/1.
//
#include "Support/Util.h"

#include <filesystem>

using namespace kelyra;
using namespace std;

std::string kelyra::GetPathString(const string & Path) {
  return filesystem::absolute(Path)
  .lexically_normal().make_preferred().string();
}

std::filesystem::path kelyra::GetPath(const string & Path) {
  return filesystem::absolute(Path)
  .lexically_normal().make_preferred();
}