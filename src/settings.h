#pragma once

#include <string>
#include <vector>

namespace settings {
struct WindowPreference {
    std::wstring name;
    unsigned slot;
    bool taskbar;
};

struct Config {
    std::vector<std::wstring> names;
    bool tabbed = true;
    bool defaultTaskbar = false;
    std::vector<WindowPreference> windows;
    bool watches(const std::wstring& name) const;
    bool taskbarFor(const std::wstring& name, unsigned slot) const;
    void setTaskbar(const std::wstring& name, unsigned slot, bool enabled);
};

std::wstring parseNames(const std::wstring& lines, Config& config);
std::wstring load(Config& config);
std::wstring save(const Config& config);
}
