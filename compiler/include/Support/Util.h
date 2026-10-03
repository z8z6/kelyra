//
// Created by zhou_zhengming on 2026/10/1.
//

#pragma once

#include <filesystem>
#include <string>

namespace kelyra {
std::string GetPathString(const std::string& path);
std::filesystem::path GetPath(const std::string& path);
}