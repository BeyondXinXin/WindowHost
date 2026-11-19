#include "appearance.h"
#include "resource.h"
#include "settings.h"
#include "target.h"

#include <windows.h>
#include <map>
#include <memory>
#include <ole2.h>
#include <algorithm>
#include <vector>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {
constexpr int clientWidth = 1920;
constexpr int clientHeight = 1080;
constexpr DWORD frameStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr wchar_t frameClass[] = L"BeyondXinXin.WindowHost.Frame";
constexpr wchar_t viewportClass[] = L"BeyondXinXin.WindowHost.Viewport";
constexpr UINT openSettings = 100;
constexpr wchar_t settingsMenu[] = L"设置(&S)…";
constexpr UINT focusTarget = WM_APP + 4;
constexpr UINT updateLayout = WM_APP + 5;
INT_PTR CALLBACK settingsProc(HWND, UINT, WPARAM, LPARAM);

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

struct Host;
struct Manager {
    HINSTANCE instance;
    settings::Config config;
    std::vector<std::unique_ptr<Host>> hosts;
    std::vector<target::Candidate> previous;
    std::map<DWORD, ULONGLONG> retries;
    std::map<DWORD, HANDLE> ignored;
    HWND dialog = nullptr;
    ULONGLONG nextScan = 0;

    ~Manager();
    Host* create(int show, HWND targetWindow = nullptr);
    bool hasOther(HWND frame, bool includeClosing = true) const;
    void poll();
    void apply(settings::Config chosen);
    void ignore(DWORD pid);
};

struct Host {
    explicit Host(Manager& owner) : manager(owner) {}
    ~Host() { if (stop) CloseHandle(stop); }

    Manager& manager;
    Appearance theme;
    HWND frame = nullptr;
    HWND viewport = nullptr;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    target::Candidate candidate;
    std::wstring notice;
    int bottomInset = 0;
    bool ready = false;
    bool closing = false;

    void applyTheme()
    {
        theme.readSystem();
        theme.setCaption(frame);
        HMENU menu = GetMenu(frame);
        MENUINFO info{sizeof(MENUINFO)};
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = theme.dark ? theme.menu() : nullptr;
        SetMenuInfo(menu, &info);
        MENUITEMINFOW item{sizeof(MENUITEMINFOW)};
        item.fMask = MIIM_FTYPE | MIIM_STRING;
        item.fType = theme.dark ? MFT_OWNERDRAW : MFT_STRING;
        item.dwTypeData = const_cast<wchar_t*>(settingsMenu);
        SetMenuItemInfoW(menu, openSettings, FALSE, &item);
        DrawMenuBar(frame);
        RedrawWindow(frame, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_NOCHILDREN);
        if (viewport)
            RedrawWindow(viewport, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
    }

    void title(const std::wstring& detail = {}) const
    {
        const std::wstring caption = detail.empty() ? L"WindowHost" : L"WindowHost — " + detail;
        SetWindowTextW(frame, caption.c_str());
    }

    void layoutViewport() const
    {
        if (!viewport)
            return;
        SetWindowPos(viewport, nullptr, 0, 0, clientWidth, clientHeight - bottomInset, SWP_NOZORDER | SWP_NOACTIVATE);
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

    void updateInset()
    {
        if (IsIconic(frame))
            return;
        int inset = 0;
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (manager.config.reserveTaskbar &&
            GetMonitorInfoW(MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &monitor))
            inset = std::clamp<int>(monitor.rcMonitor.bottom - monitor.rcWork.bottom, 0, clientHeight - 1);
        if (inset != bottomInset) {
            bottomInset = inset;
            layoutViewport();
            InvalidateRect(frame, nullptr, TRUE);
        }
    }

    void settings()
    {
        if (manager.dialog) {
            SetForegroundWindow(manager.dialog);
            return;
        }
        if (!closing && DialogBoxParamW(manager.instance, MAKEINTRESOURCEW(IDD_SETTINGS), frame,
                                        settingsProc, reinterpret_cast<LPARAM>(this)) == -1)
            MessageBoxW(frame, L"无法打开设置窗口。", L"WindowHost", MB_OK | MB_ICONERROR);
    }

    DWORD start()
    {
        const auto context = GetWindowDpiAwarenessContext(candidate.window);
        const auto previous = SetThreadDpiAwarenessContext(context);
        if (!previous)
            return ERROR_INVALID_STATE;
        viewport = CreateWindowExW(0, viewportClass, nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE,
                                   0, 0, 0, 0, frame, nullptr, GetModuleHandleW(nullptr), &theme);
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
            worker = std::thread([window = frame, pane = viewport, id = candidate, cancel = stop] {
                const DWORD result = target::host(window, pane, id, cancel);
                PostMessageW(window, target::finished, result, 0);
            });
        } catch (const std::system_error&) {
            DestroyWindow(viewport);
            viewport = nullptr;
            return ERROR_NO_SYSTEM_RESOURCES;
        }
        title(L"正在嵌入 " + candidate.name);
        return ERROR_SUCCESS;
    }

    void finish(DWORD error)
    {
        if (!worker.joinable())
            return;
        worker.join();
        ready = false;
        DestroyWindow(viewport);
        viewport = nullptr;
        notice.clear();
        if (error && error != ERROR_INVALID_WINDOW_HANDLE && error != ERROR_CANCELLED) {
            notice = candidate.name + L"  " + captureError(error);
            manager.retries[candidate.pid] = GetTickCount64() + 3000;
        }
        candidate = {};
        const bool ownsDialog = manager.dialog && GetWindow(manager.dialog, GW_OWNER) == frame;
        if (closing || (notice.empty() && !ownsDialog && manager.hasOther(frame, false))) {
            if (closing && !notice.empty())
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
            manager.ignore(candidate.pid);
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
        SetTextColor(dc, theme.text());
        const int pad = MulDiv(32, dpi, 96);
        rect.left += pad;
        rect.right -= pad;
        if (bottomInset) {
            RECT strip{0, rect.bottom - bottomInset, clientWidth, rect.bottom};
            FillRect(dc, &strip, theme.dark ? theme.menu() : GetSysColorBrush(COLOR_BTNFACE));
            rect.bottom -= bottomInset;
        }
        const LONG center = rect.bottom / 2;
        RECT status = rect;
        status.bottom = center;
        std::wstring label = notice;
        if (label.empty())
            label = manager.config.names.empty() ? L"请在设置中添加进程名" : L"正在等待目标进程";
        DrawTextW(dc, label.c_str(), -1, &status, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!manager.config.names.empty()) {
            std::wstring names;
            for (const auto& name : manager.config.names) {
                if (!names.empty()) names += L"  ·  ";
                names += name;
            }
            rect.top = center + MulDiv(8, dpi, 96);
            SetTextColor(dc, theme.secondary());
            DrawTextW(dc, names.c_str(), -1, &rect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
        if (textFont)
            DeleteObject(textFont);
        EndPaint(frame, &paint);
    }
};

INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    auto* app = reinterpret_cast<Host*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        app = reinterpret_cast<Host*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        app->theme.setCaption(dialog);
        app->manager.dialog = dialog;
        std::wstring names;
        for (const auto& name : app->manager.config.names)
            names += name + L"\r\n";
        SendDlgItemMessageW(dialog, IDC_PROCESS_NAMES, EM_SETLIMITTEXT, 32767, 0);
        SetDlgItemTextW(dialog, IDC_PROCESS_NAMES, names.c_str());
        CheckDlgButton(dialog, IDC_TASKBAR, app->manager.config.reserveTaskbar ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    if (!app)
        return FALSE;
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            HWND edit = GetDlgItem(dialog, IDC_PROCESS_NAMES);
            std::wstring lines(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
            const int copied = GetWindowTextW(edit, lines.data(), static_cast<int>(lines.size()));
            lines.resize(copied);
            settings::Config chosen;
            chosen.reserveTaskbar = IsDlgButtonChecked(dialog, IDC_TASKBAR) == BST_CHECKED;
            auto error = settings::parseNames(lines, chosen);
            if (error.empty())
                error = settings::save(chosen);
            if (error.empty()) {
                app->manager.apply(std::move(chosen));
                EndDialog(dialog, IDOK);
            } else {
                MessageBoxW(dialog, error.c_str(), L"WindowHost", MB_OK | MB_ICONWARNING);
                SetFocus(edit);
            }
            return TRUE;
        }
        break;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(app->theme.background());
    case WM_DESTROY:
        app->manager.dialog = nullptr;
        break;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wp), app->theme.text());
        return reinterpret_cast<INT_PTR>(app->theme.background());
    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(wp), app->theme.fieldColor());
        SetTextColor(reinterpret_cast<HDC>(wp), app->theme.text());
        return reinterpret_cast<INT_PTR>(app->theme.field());
    case WM_DRAWITEM:
        if (wp == IDOK || wp == IDCANCEL) {
            app->theme.drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        app->theme.readSystem();
        app->theme.setCaption(dialog);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        break;
    }
    return FALSE;
}

LRESULT CALLBACK viewportProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    if (message == WM_ERASEBKGND) {
        const auto* theme = reinterpret_cast<const Appearance*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (theme) {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wp), &rect, theme->background());
            return 1;
        }
    }
    return DefWindowProcW(window, message, wp, lp);
}

LRESULT CALLBACK frameProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* app = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->frame = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_COMMAND:
        if (!lp && LOWORD(wp) == openSettings)
            app->settings();
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        app->applyTheme();
        app->updateInset();
        break;
    case WM_DISPLAYCHANGE:
    case WM_MOVE:
        app->updateInset();
        break;
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wp), &rect, app->theme.background());
        return 1;
    }
    case WM_MEASUREITEM: {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_MENU && item.itemID == openSettings) {
            measureDarkMenu(window, item, settingsMenu);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_MENU && item.itemID == openSettings) {
            drawDarkMenu(window, item, settingsMenu, app->theme);
            return TRUE;
        }
        break;
    }
    case WM_MENUCHAR:
        if (app->theme.dark && reinterpret_cast<HMENU>(lp) == GetMenu(window)) {
            if (LOWORD(wp) == L's' || LOWORD(wp) == L'S')
                return MAKELRESULT(0, MNC_EXECUTE);
        }
        break;
    case WM_TIMER:
        if (wp == 1 && !app->closing) {
            app->updateInset();
            app->manager.poll();
        }
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
        app->updateInset();
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
            app->title(app->candidate.name + L"  [" + std::to_wstring(app->candidate.pid) + L"]");
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
        KillTimer(window, 1);
        if (!app->manager.hasOther(window))
            PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        app->frame = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

Manager::~Manager()
{
    for (const auto& entry : ignored)
        CloseHandle(entry.second);
}

bool Manager::hasOther(HWND frame, bool includeClosing) const
{
    return std::any_of(hosts.begin(), hosts.end(), [frame, includeClosing](const auto& host) {
        return host->frame && host->frame != frame && (includeClosing || !host->closing);
    });
}

void Manager::ignore(DWORD pid)
{
    if (ignored.count(pid))
        return;
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid))
        ignored.emplace(pid, process);
}

void Manager::apply(settings::Config chosen)
{
    config = std::move(chosen);
    previous.clear();
    retries.clear();
    nextScan = 0;
    for (auto& host : hosts) {
        if (!host->frame || host->closing)
            continue;
        host->notice.clear();
        host->updateInset();
        if (host->worker.joinable() && !config.watches(host->candidate.name)) {
            host->ready = false;
            SetEvent(host->stop);
            host->title(L"正在释放目标窗口");
        }
        InvalidateRect(host->frame, nullptr, TRUE);
    }
}

Host* Manager::create(int show, HWND targetWindow)
{
    auto host = std::make_unique<Host>(*this);
    if (!host->stop)
        return nullptr;
    HMENU menu = CreateMenu();
    if (!menu || !AppendMenuW(menu, MF_STRING, openSettings, settingsMenu)) {
        if (menu) DestroyMenu(menu);
        return nullptr;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR display = targetWindow ? MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST)
                                    : MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(display, &monitor);
    const RECT work = monitor.rcWork;
    Host* app = host.get();
    hosts.push_back(std::move(host));
    const auto previousDpi = SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
    HWND window = nullptr;
    if (previousDpi != DPI_HOSTING_BEHAVIOR_INVALID) {
        window = CreateWindowExW(0, frameClass, L"WindowHost", frameStyle,
                                 work.left, work.top, clientWidth, clientHeight,
                                 nullptr, menu, instance, app);
        SetThreadDpiHostingBehavior(previousDpi);
    }
    if (!window) {
        DestroyMenu(menu);
        return nullptr;
    }
    app->applyTheme();
    const auto size = frameSize(GetDpiForWindow(window));
    const LONG cascade = static_cast<LONG>((hosts.size() - 1) % 6) * 24;
    const LONG availableX = std::max(0L, work.right - work.left - size.cx);
    const LONG availableY = std::max(0L, work.bottom - work.top - size.cy);
    const LONG x = work.left + std::min(availableX, availableX / 2 + cascade);
    const LONG y = work.top + std::min(availableY, availableY / 2 + cascade);
    SetWindowPos(window, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    if (!SetTimer(window, 1, 600, nullptr)) {
        DestroyWindow(window);
        return nullptr;
    }
    ShowWindow(window, show == SW_SHOWMAXIMIZED ? SW_SHOWNORMAL : show);
    return app;
}

void Manager::poll()
{
    const auto now = GetTickCount64();
    if (dialog || now < nextScan)
        return;
    nextScan = now + 500;
    for (auto it = ignored.begin(); it != ignored.end();) {
        if (WaitForSingleObject(it->second, 0) == WAIT_TIMEOUT) {
            ++it;
        } else {
            CloseHandle(it->second);
            it = ignored.erase(it);
        }
    }
    for (auto it = retries.begin(); it != retries.end();) {
        if (it->second <= now)
            it = retries.erase(it);
        else
            ++it;
    }
    if (config.names.empty())
        return;

    auto found = target::findMainWindows(config.names);
    for (const auto& candidate : found) {
        if (ignored.count(candidate.pid) || retries.count(candidate.pid))
            continue;
        const bool stable = std::any_of(previous.begin(), previous.end(), [&](const auto& earlier) {
            return candidate.pid == earlier.pid && candidate.thread == earlier.thread && candidate.window == earlier.window;
        });
        if (!stable)
            continue;
        Host* idle = nullptr;
        bool bound = false;
        for (const auto& host : hosts) {
            if (host->worker.joinable() && host->candidate.pid == candidate.pid) {
                bound = true;
                break;
            }
            if (!idle && host->frame && !host->closing && !host->worker.joinable())
                idle = host.get();
        }
        if (bound)
            continue;
        if (!idle)
            idle = create(SW_SHOWNOACTIVATE, candidate.window);
        if (!idle) {
            retries[candidate.pid] = now + 3000;
            for (const auto& host : hosts) {
                if (host->frame && !host->closing) {
                    host->title(L"无法为 " + candidate.name + L" 创建窗口");
                    break;
                }
            }
            continue;
        }
        idle->candidate = candidate;
        idle->notice.clear();
        const DWORD error = idle->start();
        if (error) {
            retries[candidate.pid] = now + 3000;
            idle->notice = candidate.name + L"  " + captureError(error);
            idle->candidate = {};
            InvalidateRect(idle->frame, nullptr, TRUE);
        }
    }
    previous = std::move(found);
}

int run(HINSTANCE instance, int show)
{
    WNDCLASSEXW cls{sizeof(WNDCLASSEXW)};
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_WINDOWHOST));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_WINDOWHOST), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    cls.lpfnWndProc = frameProc;
    cls.lpszClassName = frameClass;
    if (!RegisterClassExW(&cls))
        return 1;
    cls.lpfnWndProc = viewportProc;
    cls.lpszClassName = viewportClass;
    if (!RegisterClassExW(&cls))
        return 1;

    Manager manager{instance};
    const auto notice = settings::load(manager.config);
    Host* first = manager.create(show);
    if (!first)
        return 1;
    first->notice = notice;
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // Keep Host objects alive until the entire window callback has unwound.
        manager.hosts.erase(std::remove_if(manager.hosts.begin(), manager.hosts.end(),
            [](const auto& host) { return !host->frame; }), manager.hosts.end());
    }
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
