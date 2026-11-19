#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <cwchar>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace settings {
using std::filesystem::path;
namespace {
path configFile()
{
    PWSTR folder = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
    if (FAILED(hr))
        return {};
    path file = path(folder) / L"BeyondXinXin" / L"WindowHost" / L"target.txt";
    CoTaskMemFree(folder);
    return file;
}
}

static bool isProcessName(const path& target)
{
    const auto& name = target.native();
    if (name.empty() || name.size() > 255 || name.find_first_of(L"\\/:*?\"<>|") != name.npos ||
        _wcsicmp(target.extension().c_str(), L".exe") != 0)
        return false;
    for (wchar_t ch : name) {
        if (ch < L' ')
            return false;
    }
    return true;
}

bool Config::watches(const std::wstring& name) const
{
    for (const auto& existing : names) {
        if (CompareStringOrdinal(name.c_str(), -1, existing.c_str(), -1, TRUE) == CSTR_EQUAL)
            return true;
    }
    return false;
}

std::wstring parseNames(const std::wstring& lines, Config& config)
{
    Config parsed;
    wchar_t self[32768]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    const auto selfName = path(self).filename().wstring();
    std::wistringstream input(lines);
    std::wstring line;
    unsigned row = 0;
    while (std::getline(input, line)) {
        ++row;
        const auto first = line.find_first_not_of(L" \t\r");
        if (first == line.npos)
            continue;
        auto name = line.substr(first, line.find_last_not_of(L" \t\r") - first + 1);
        if (!isProcessName(name))
            return L"第 " + std::to_wstring(row) + L" 行需要填写 EXE 文件名，例如 Navigation.exe。";
        if (CompareStringOrdinal(name.c_str(), -1, selfName.c_str(), -1, TRUE) == CSTR_EQUAL)
            return L"目标列表中包含了 WindowHost 自身。";
        if (!parsed.watches(name))
            parsed.names.push_back(std::move(name));
    }
    config.names = std::move(parsed.names);
    return {};
}

std::wstring load(Config& config)
{
    const path file = configFile();
    if (file.empty())
        return L"无法定位本机应用数据目录。";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec)
        return {};
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > 262144)
        return L"无法读取目标配置，请打开设置。";
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return L"无法打开目标配置，请检查目录权限。";
    std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad())
        return L"读取目标配置失败。";
    if (value.compare(0, 3, "\xEF\xBB\xBF") == 0)
        value.erase(0, 3);
    try {
        std::string line;
        std::istringstream input(value);
        std::wstring names;
        Config parsed;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == "reserveTaskbar=1" || line == "reserveTaskbar=0") {
                parsed.reserveTaskbar = line.back() == '1';
            } else {
                // The first release saved a single absolute executable path.
                const path entry = std::filesystem::u8path(line);
                names += (entry.is_absolute() ? entry.filename() : entry).wstring() + L'\n';
            }
        }
        auto error = parseNames(names, parsed);
        if (error.empty())
            config = std::move(parsed);
        return error;
    } catch (const std::system_error&) {
        // Malformed UTF-8 in a manually edited file.
    }
    return L"目标配置无效，请打开设置。";
}

std::wstring save(const Config& config)
{
    const path file = configFile();
    if (file.empty())
        return L"无法定位本机应用数据目录。";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec)
        return L"无法创建配置目录。\n" + file.parent_path().wstring();
    path temp = file;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
        return L"无法写入配置目录。\n" + file.parent_path().wstring();
    out << "reserveTaskbar=" << (config.reserveTaskbar ? "1" : "0") << '\n';
    for (const auto& name : config.names)
        out << path(name).u8string() << '\n';
    out.close();
    if (!out || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return L"无法保存目标配置。\n" + file.wstring();
    }
    return {};
}
}
