#pragma once

#include <string>
#include <vector>

namespace settings {
struct Config {
    std::vector<std::wstring> names;
    bool reserveTaskbar = false;
    bool watches(const std::wstring& name) const;
};

std::wstring parseNames(const std::wstring& lines, Config& config);
std::wstring load(Config& config);
std::wstring save(const Config& config);
}
