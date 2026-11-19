#include "target.h"

#include <dwmapi.h>
#include <cstdint>
#include <iterator>
#include <map>
#include <string_view>

namespace target {
namespace {
std::wstring imageName(HANDLE process)
{
    wchar_t image[32768];
    DWORD length = static_cast<DWORD>(std::size(image));
    if (!QueryFullProcessImageNameW(process, 0, image, &length))
        return {};
    std::wstring_view actual(image, length);
    const auto separator = actual.find_last_of(L"\\/");
    if (separator != actual.npos)
        actual.remove_prefix(separator + 1);
    return std::wstring(actual);
}

struct ProcessWindows {
    Candidate best;
    std::int64_t area = 0;
};

struct Search {
    const std::vector<std::wstring>& names;
    std::map<DWORD, ProcessWindows> processes;
};

BOOL CALLBACK consider(HWND window, LPARAM param)
{
    auto& search = *reinterpret_cast<Search*>(param);
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &pid);
    if (!thread || pid == GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window) ||
        GetWindow(window, GW_OWNER) || (GetWindowLongW(window, GWL_STYLE) & WS_CHILD) ||
        (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;

    auto [it, inserted] = search.processes.try_emplace(pid);
    auto& record = it->second;
    if (inserted) {
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            const auto actual = imageName(process);
            CloseHandle(process);
            for (const auto& name : search.names) {
                if (CompareStringOrdinal(name.c_str(), -1, actual.c_str(), -1, TRUE) == CSTR_EQUAL) {
                    record.best.name = name;
                    break;
                }
            }
        }
    }
    if (record.best.name.empty())
        return TRUE;
    DWORD cloaked = 0;
    RECT client{};
    if ((SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) ||
        !GetClientRect(window, &client))
        return TRUE;
    const auto area = static_cast<std::int64_t>(client.right) * client.bottom;
    if (area > record.area) {
        record.area = area;
        record.best.window = window;
        record.best.pid = pid;
        record.best.thread = thread;
    }
    return TRUE;
}

DWORD lastError()
{
    const DWORD error = GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
}

constexpr DWORD frameBits = WS_CHILD | WS_POPUP | WS_CAPTION | WS_THICKFRAME |
                            WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MINIMIZE | WS_MAXIMIZE;
constexpr DWORD edgeBits = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                           WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE;
constexpr UINT positionFlags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;

// Only modify the bits we own. In particular Qt may disable the main window
// while a modal dialog is open or hide it while replacing its native handle.
DWORD setBits(HWND window, int index, DWORD mask, DWORD bits, bool& changed)
{
    SetLastError(ERROR_SUCCESS);
    const DWORD original = static_cast<DWORD>(GetWindowLongW(window, index));
    if (!original && GetLastError())
        return lastError();
    const DWORD desired = (original & ~mask) | (bits & mask);
    if (desired != original) {
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongW(window, index, static_cast<LONG>(desired)) && GetLastError())
            return lastError();
        changed = true;
    }
    return ERROR_SUCCESS;
}

DWORD changeParent(HWND window, HWND parent)
{
    SetLastError(ERROR_SUCCESS);
    if (!SetParent(window, parent) && GetLastError())
        return lastError();
    return ERROR_SUCCESS;
}

struct Session {
    HWND notify;
    HWND viewport;
    Candidate id;
    HANDLE process;
    HANDLE stop;
    WINDOWPLACEMENT placement{};
    DWORD style = 0;
    DWORD exStyle = 0;
    bool dirty = false;

    bool alive() const
    {
        DWORD pid = 0;
        return WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
               GetWindowThreadProcessId(id.window, &pid) == id.thread && pid == id.pid;
    }

    DWORD childStyles(bool& changed) const
    {
        DWORD error = setBits(id.window, GWL_STYLE, frameBits, WS_CHILD, changed);
        if (!error)
            error = setBits(id.window, GWL_EXSTYLE, edgeBits, 0, changed);
        return error;
    }

    DWORD fit(bool changed) const
    {
        RECT desired{}, actual{};
        if (!GetClientRect(viewport, &desired) || !GetWindowRect(id.window, &actual))
            return lastError();
        SetLastError(ERROR_SUCCESS);
        if (!MapWindowPoints(HWND_DESKTOP, viewport, reinterpret_cast<POINT*>(&actual), 2) && GetLastError())
            return lastError();
        const bool topmost = (GetWindowLongW(id.window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (!changed && !topmost && EqualRect(&actual, &desired))
            return ERROR_SUCCESS;
        UINT flags = positionFlags;
        if (changed)
            flags |= SWP_FRAMECHANGED;
        if (!topmost)
            flags |= SWP_NOZORDER;
        return SetWindowPos(id.window, HWND_NOTOPMOST, 0, 0, desired.right, desired.bottom, flags)
                   ? ERROR_SUCCESS : lastError();
    }

    DWORD attach()
    {
        const HWND window = id.window;
        if (!alive() || GetAncestor(window, GA_PARENT) != GetDesktopWindow() || GetWindow(window, GW_OWNER) ||
            !IsWindowVisible(window) || (GetWindowLongW(window, GWL_STYLE) & WS_CHILD))
            return ERROR_INVALID_WINDOW_HANDLE;
        if (!AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(viewport),
                                          GetWindowDpiAwarenessContext(window)))
            return ERROR_INVALID_STATE;
        // Probe from the worker's independent queue before changing a paused target.
        if (!SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 180, nullptr))
            return ERROR_TIMEOUT;
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || !alive())
            return ERROR_CANCELLED;
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(window, &placement))
            return lastError();
        style = static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE));
        exStyle = static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE));
        if (exStyle & WS_EX_TOPMOST) {
            if (!SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, positionFlags | SWP_NOMOVE | SWP_NOSIZE))
                return lastError();
            dirty = true;
        }
        DWORD error = childStyles(dirty);
        if (error)
            return error;
        error = changeParent(window, viewport);
        if (error)
            return error;
        dirty = true;
        if (GetAncestor(window, GA_PARENT) != viewport)
            return ERROR_INVALID_STATE;
        return fit(true);
    }

    DWORD follow() const
    {
        PostMessageW(notify, attached, 0, 0);
        const HANDLE signals[] = {stop, process};
        unsigned hiddenTicks = 0;
        while (WaitForMultipleObjects(2, signals, FALSE, 400) == WAIT_TIMEOUT && alive()) {
            if (GetAncestor(id.window, GA_PARENT) != viewport)
                break;
            if (!(GetWindowLongW(id.window, GWL_STYLE) & WS_VISIBLE)) {
                if (++hiddenTicks >= 3)
                    break;
                continue;
            }
            hiddenTicks = 0;
            if (IsIconic(GetAncestor(viewport, GA_ROOT)))
                continue;
            bool changed = false;
            DWORD error = childStyles(changed);
            if (!error)
                error = fit(changed);
            if (error)
                return error;
        }
        return ERROR_SUCCESS;
    }

    DWORD restore() const
    {
        if (!dirty || !alive())
            return ERROR_SUCCESS;
        const HWND window = id.window;
        HWND parent = GetAncestor(window, GA_PARENT);
        if (parent != viewport && parent != GetDesktopWindow())
            return ERROR_SUCCESS; // The application has already moved this window elsewhere.
        // Never let the UI destroy the viewport while it still owns the target.
        bool notified = false;
        while (parent == viewport) {
            changeParent(window, nullptr);
            if (!alive())
                return ERROR_SUCCESS;
            parent = GetAncestor(window, GA_PARENT);
            if (parent == viewport) {
                if (!notified)
                    PostMessageW(notify, restoring, 0, 0);
                notified = true;
                WaitForSingleObject(process, 500);
            }
        }
        if (parent != GetDesktopWindow() || !alive())
            return ERROR_SUCCESS;

        bool changed = false;
        DWORD error = setBits(window, GWL_STYLE, frameBits, style, changed);
        const DWORD edgeError = setBits(window, GWL_EXSTYLE, edgeBits, exStyle, changed);
        WINDOWPLACEMENT restored = placement;
        if (!(GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE))
            restored.showCmd = SW_HIDE;
        else if (restored.showCmd == SW_SHOWNORMAL)
            restored.showCmd = SW_SHOWNOACTIVATE;
        if (!SetWindowPlacement(window, &restored) && !error)
            error = lastError();
        const bool topmost = (exStyle & WS_EX_TOPMOST) != 0;
        UINT flags = positionFlags | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED;
        if (!topmost && !(GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST))
            flags |= SWP_NOZORDER;
        if (!SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags) && !error)
            error = lastError();
        return error ? error : edgeError;
    }
};
}

std::vector<Candidate> findMainWindows(const std::vector<std::wstring>& names)
{
    Search search{names, {}};
    EnumWindows(consider, reinterpret_cast<LPARAM>(&search));
    std::vector<Candidate> result;
    for (const auto& entry : search.processes) {
        if (entry.second.best.window)
            result.push_back(entry.second.best);
    }
    return result;
}

DWORD host(HWND notify, HWND viewport, const Candidate& candidate, HANDLE stop)
{
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.pid);
    if (!process)
        return lastError();
    DWORD result = ERROR_INVALID_WINDOW_HANDLE;
    const auto actual = imageName(process);
    if (CompareStringOrdinal(actual.c_str(), -1, candidate.name.c_str(), -1, TRUE) == CSTR_EQUAL) {
        const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(viewport));
        if (previous) {
            Session session{notify, viewport, candidate, process, stop};
            result = session.attach();
            if (!result)
                result = session.follow();
            const DWORD restoreError = session.restore();
            if (!session.alive())
                result = ERROR_SUCCESS;
            else if (restoreError)
                result = restoreError;
            SetThreadDpiAwarenessContext(previous);
        } else {
            result = lastError();
        }
    }
    CloseHandle(process);
    return result;
}
}
