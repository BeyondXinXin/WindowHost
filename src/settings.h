#pragma once

#include <filesystem>
#include <string>

namespace settings {
using std::filesystem::path;

// An empty target means the user has not selected an executable yet.
std::wstring load(path& target);
std::wstring save(const path& target);
}
