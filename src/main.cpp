#include "settings.h"
#include "target.h"

#include <windows.h>
#include <commdlg.h>
#include <ole2.h>
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

namespace {
constexpr int clientWidth = 1920;
constexpr int clientHeight = 1080;
constexpr DWORD frameStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr wchar_t frameClass[] = L"BeyondXinXin.WindowHost.Frame";
constexpr wchar_t viewportClass[] = L"BeyondXinXin.WindowHost.Viewport";
constexpr UINT chooseExe = 100;
constexpr UINT focusTarget = WM_APP + 4;
constexpr UINT updateLayout = WM_APP + 5;

SIZE frameSize(UINT dpi)
{
    RECT rect{0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&rect, frameStyle, TRUE, 0, dpi ? dpi : 96);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

std::wstring captureError(DWORD code)
{
    if (code == ERROR_ACCESS_DENIED)
        return L"访问被拒绝，请以与目标程序相同的权限运行 WindowHost。";
    if (code == ERROR_TIMEOUT)
        return L"正在等待目标窗口响应。若停在断点，请在 Qt Creator 中继续运行。";
    return L"未能完成窗口托管，系统错误 " + std::to_wstring(code) + L"。";
}

struct App {
    HWND frame = nullptr;
    HWND viewport = nullptr;
    HANDLE stop = nullptr;
    std::thread worker;
    std::filesystem::path exe;
    target::Candidate candidate;
    std::wstring notice;
    ULONGLONG retryAt = 0;
    bool picking = false;
    bool ready = false;
    bool closing = false;

    void title(const std::wstring& detail = {}) const
    {
        const std::wstring caption = detail.empty() ? L"WindowHost" : L"WindowHost — " + detail;
        SetWindowTextW(frame, caption.c_str());
    }

    void layoutViewport() const
    {
        if (!viewport)
            return;
        SetWindowPos(viewport, nullptr, 0, 0, clientWidth, clientHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void correctClientSize() const
    {
        RECT client{}, outer{};
        if (IsIconic(frame) || !GetClientRect(frame, &client) ||
            (client.right == clientWidth && client.bottom == clientHeight) || !GetWindowRect(frame, &outer))
            return;
        // Measure the real menu and caption after Windows has laid them out.
        const int width = outer.right - outer.left + clientWidth - client.right;
        const int height = outer.bottom - outer.top + clientHeight - client.bottom;
        SetWindowPos(frame, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }

    void selectExe()
    {
        if (picking || closing)
            return;
        picking = true;
        wchar_t buffer[32768]{};
        const std::wstring directory = exe.parent_path().wstring();
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = frame;
        dialog.lpstrFilter = L"Windows 可执行文件 (*.exe)\0*.exe\0";
        dialog.lpstrTitle = L"选择目标 EXE";
        dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        std::wstring error;
        if (GetOpenFileNameW(&dialog)) {
            const std::filesystem::path chosen(buffer);
            wchar_t self[32768]{};
            GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
            std::error_code ec;
            if (_wcsicmp(chosen.extension().c_str(), L".exe") != 0)
                error = L"请选择扩展名为 .exe 的程序。";
            else if (std::filesystem::equivalent(chosen, std::filesystem::path(self), ec))
                error = L"请选择要调试的目标程序。";
            else
                error = settings::save(chosen);
            if (error.empty()) {
                if (worker.joinable() && CompareStringOrdinal(exe.c_str(), -1, chosen.c_str(), -1, TRUE) != CSTR_EQUAL) {
                    ready = false;
                    SetEvent(stop);
                    title(L"正在切换目标");
                }
                exe = chosen;
                if (!worker.joinable())
                    candidate = {};
                notice.clear();
                retryAt = 0;
                InvalidateRect(frame, nullptr, TRUE);
            }
        } else if (const DWORD code = CommDlgExtendedError()) {
            error = L"无法打开文件选择窗口，系统错误 " + std::to_wstring(code) + L"。";
        }
        if (!error.empty())
            MessageBoxW(frame, error.c_str(), L"WindowHost", MB_OK | MB_ICONERROR);
        picking = false;
    }

    DWORD start()
    {
        const auto context = GetWindowDpiAwarenessContext(candidate.window);
        const auto previous = SetThreadDpiAwarenessContext(context);
        if (!previous)
            return ERROR_INVALID_STATE;
        viewport = CreateWindowExW(0, viewportClass, nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE,
                                   0, 0, 0, 0, frame, nullptr, GetModuleHandleW(nullptr), nullptr);
        const DWORD error = viewport ? ERROR_SUCCESS : GetLastError();
        SetThreadDpiAwarenessContext(previous);
        if (!viewport)
            return error ? error : ERROR_GEN_FAILURE;
        if (!AreDpiAwarenessContextsEqual(context, GetWindowDpiAwarenessContext(viewport))) {
            DestroyWindow(viewport);
            viewport = nullptr;
            return ERROR_INVALID_STATE;
        }
        layoutViewport();
        ResetEvent(stop);
        try {
            worker = std::thread([window = frame, pane = viewport, id = candidate, path = exe, cancel = stop] {
                const DWORD result = target::host(window, pane, id, path, cancel);
                PostMessageW(window, target::finished, result, 0);
            });
        } catch (const std::system_error&) {
            DestroyWindow(viewport);
            viewport = nullptr;
            return ERROR_NO_SYSTEM_RESOURCES;
        }
        title(L"正在嵌入 " + exe.filename().wstring());
        return ERROR_SUCCESS;
    }

    void poll()
    {
        if (worker.joinable() || exe.empty() || picking || closing || GetTickCount64() < retryAt)
            return;
        const auto found = target::findMainWindow(exe);
        // Require two consecutive observations to let short-lived startup windows pass.
        if (found.window && found.window == candidate.window &&
            found.pid == candidate.pid && found.thread == candidate.thread) {
            const DWORD error = start();
            if (error) {
                notice = captureError(error);
                retryAt = GetTickCount64() + 3000;
                InvalidateRect(frame, nullptr, TRUE);
            }
        } else if (!notice.empty()) {
            notice.clear();
            InvalidateRect(frame, nullptr, TRUE);
        }
        candidate = found;
    }

    void finish(DWORD error)
    {
        if (!worker.joinable())
            return;
        worker.join();
        ready = false;
        DestroyWindow(viewport);
        viewport = nullptr;
        candidate = {};
        notice.clear();
        if (error && error != ERROR_INVALID_WINDOW_HANDLE && error != ERROR_CANCELLED) {
            notice = captureError(error);
            retryAt = GetTickCount64() + 3000;
        }
        if (closing) {
            if (!notice.empty())
                MessageBoxW(frame, notice.c_str(), L"WindowHost", MB_OK | MB_ICONWARNING);
            DestroyWindow(frame);
        } else {
            title();
            InvalidateRect(frame, nullptr, TRUE);
        }
    }

    void close()
    {
        if (closing)
            return;
        closing = true;
        KillTimer(frame, 1);
        if (worker.joinable()) {
            ready = false;
            SetEvent(stop);
            title(L"正在释放目标窗口，若停在断点请继续运行");
        } else {
            DestroyWindow(frame);
        }
    }

    void forwardFocus() const
    {
        if (!ready || !viewport || GetForegroundWindow() != frame || GetFocus() != frame)
            return;
        HWND window = candidate.window;
        if (IsChild(viewport, window) && IsWindowEnabled(window) && IsWindowVisible(window) &&
            !IsHungAppWindow(window))
            SetFocus(window);
    }

    void paint() const
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(frame, &paint);
        RECT rect{};
        GetClientRect(frame, &rect);
        const UINT dpi = GetDpiForWindow(frame);
        LOGFONTW font{};
        HFONT textFont = nullptr;
        if (SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(font), &font, 0, dpi))
            textFont = CreateFontIndirectW(&font);
        HGDIOBJ oldFont = SelectObject(dc, textFont ? static_cast<HGDIOBJ>(textFont) : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        const int pad = MulDiv(32, dpi, 96);
        rect.left += pad;
        rect.right -= pad;
        const LONG center = rect.bottom / 2;
        RECT status = rect;
        status.bottom = center;
        std::wstring label = notice;
        if (label.empty())
            label = exe.empty() ? L"请从菜单选择目标 EXE" : L"正在等待 " + exe.filename().wstring() + L" 启动";
        DrawTextW(dc, label.c_str(), -1, &status, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!exe.empty()) {
            rect.top = center + MulDiv(8, dpi, 96);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            DrawTextW(dc, exe.c_str(), -1, &rect, DT_CENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
        if (textFont)
            DeleteObject(textFont);
        EndPaint(frame, &paint);
    }
};

LRESULT CALLBACK frameProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->frame = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wp) == chooseExe && !lp)
            app->selectExe();
        return 0;
    case WM_TIMER:
        if (wp == 1)
            app->poll();
        return 0;
    case WM_PAINT:
        app->paint();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        const auto size = frameSize(GetDpiForWindow(window));
        info->ptMinTrackSize = info->ptMaxTrackSize = {size.cx, size.cy};
        return 0;
    }
    case WM_GETDPISCALEDSIZE:
        *reinterpret_cast<SIZE*>(lp) = frameSize(static_cast<UINT>(wp));
        return TRUE;
    case WM_DPICHANGED: {
        const auto& proposed = *reinterpret_cast<const RECT*>(lp);
        const auto size = frameSize(LOWORD(wp));
        SetWindowPos(window, nullptr, proposed.left, proposed.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        // Lay out mixed-DPI children after Windows finishes the DPI transition.
        PostMessageW(window, updateLayout, 0, 0);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            PostMessageW(window, updateLayout, 0, 0);
        return 0;
    case updateLayout:
        app->correctClientSize();
        app->layoutViewport();
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SIZE || (wp & 0xFFF0) == SC_MAXIMIZE)
            return 0;
        break;
    case target::attached:
        if (!app->closing && WaitForSingleObject(app->stop, 0) == WAIT_TIMEOUT) {
            app->ready = true;
            app->title(app->exe.filename().wstring());
            PostMessageW(window, focusTarget, 0, 0);
        }
        return 0;
    case target::restoring:
        app->ready = false;
        app->title(L"正在等待目标释放窗口");
        return 0;
    case target::finished:
        app->finish(static_cast<DWORD>(wp));
        return 0;
    case WM_SETFOCUS:
        PostMessageW(window, focusTarget, 0, 0);
        break;
    case focusTarget:
        app->forwardFocus();
        return 0;
    case WM_CLOSE:
        app->close();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int run(HINSTANCE instance, int show)
{
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    cls.lpfnWndProc = frameProc;
    cls.lpszClassName = frameClass;
    if (!RegisterClassW(&cls))
        return 1;
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = viewportClass;
    if (!RegisterClassW(&cls))
        return 1;

    App app;
    app.notice = settings::load(app.exe);
    app.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!app.stop)
        return 1;
    HMENU menu = CreateMenu();
    if (!menu || !AppendMenuW(menu, MF_STRING, chooseExe, L"选择目标 EXE(&T)…")) {
        if (menu)
            DestroyMenu(menu);
        CloseHandle(app.stop);
        return 1;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const auto previous = SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
    HWND window = nullptr;
    if (previous != DPI_HOSTING_BEHAVIOR_INVALID) {
        window = CreateWindowExW(0, frameClass, L"WindowHost", frameStyle,
                                 work.left, work.top, clientWidth, clientHeight,
                                 nullptr, menu, instance, &app);
        SetThreadDpiHostingBehavior(previous);
    }
    if (!window) {
        DestroyMenu(menu);
        CloseHandle(app.stop);
        return 1;
    }

    const auto size = frameSize(GetDpiForWindow(window));
    const LONG x = work.left + std::max(0L, (work.right - work.left - size.cx) / 2);
    const LONG y = work.top + std::max(0L, (work.bottom - work.top - size.cy) / 2);
    SetWindowPos(window, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    if (!SetTimer(window, 1, 600, nullptr)) {
        DestroyWindow(window);
        CloseHandle(app.stop);
        return 1;
    }
    ShowWindow(window, show == SW_SHOWMAXIMIZED ? SW_SHOWNORMAL : show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(app.stop);
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        MessageBoxW(nullptr, L"无法启用逐显示器 DPI 感知。", L"WindowHost", MB_ICONERROR);
        return 1;
    }
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\BeyondXinXin.WindowHost");
    if (!singleton)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND previous = FindWindowW(frameClass, nullptr)) {
            if (IsIconic(previous))
                ShowWindowAsync(previous, SW_RESTORE);
            SetForegroundWindow(previous);
        }
        CloseHandle(singleton);
        return 0;
    }
    const HRESULT ole = OleInitialize(nullptr);
    const int result = run(instance, show);
    if (result)
        MessageBoxW(nullptr, L"无法创建 WindowHost 窗口。", L"WindowHost", MB_ICONERROR);
    if (SUCCEEDED(ole))
        OleUninitialize();
    CloseHandle(singleton);
    return result;
}
