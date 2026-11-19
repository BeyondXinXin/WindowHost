#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <system_error>

namespace settings {
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

std::wstring load(path& target)
{
    const path file = configFile();
    if (file.empty())
        return L"无法定位本机应用数据目录。";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec)
        return {};
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > 131072)
        return L"无法读取目标配置，请重新选择 EXE。";
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return L"无法打开目标配置，请检查目录权限。";
    std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad())
        return L"读取目标配置失败。";
    if (value.compare(0, 3, "\xEF\xBB\xBF") == 0)
        value.erase(0, 3);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
        value.pop_back();
    try {
        const path chosen = std::filesystem::u8path(value);
        if (value.find('\0') == std::string::npos && chosen.is_absolute() &&
            _wcsicmp(chosen.extension().c_str(), L".exe") == 0) {
            target = chosen;
            return {};
        }
    } catch (const std::system_error&) {
        // Malformed UTF-8 in a manually edited file.
    }
    return L"目标配置无效，请重新选择 EXE。";
}

std::wstring save(const path& target)
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
    out << target.u8string() << '\n';
    out.close();
    if (!out || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return L"无法保存目标配置。\n" + file.wstring();
    }
    return {};
}
}
