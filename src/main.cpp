#include "pane.h"
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
#include <utility>

namespace {
constexpr int clientWidth = 1920;
constexpr int clientHeight = 1080;
constexpr DWORD frameStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr wchar_t frameClass[] = L"BeyondXinXin.WindowHost.Frame";
constexpr UINT firstTab = 1000;
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
    HWND groupFrame = nullptr;
    bool pendingLayout = false;
    Host* group() const;
    bool arrange(Host& primary);
    void apply(settings::Config chosen, Host& origin);
    unsigned nextSlot(const std::wstring& name) const;
    void ignore(DWORD pid);
};

struct DialogState {
    Host& owner;
    settings::Config chosen;
    std::wstring name;
    unsigned slot = 0;
    std::wstring label;
};

struct Host {
    explicit Host(Manager& owner) : manager(owner) {}

    Manager& manager;
    Appearance theme;
    HWND frame = nullptr;
    std::vector<std::unique_ptr<Pane>> panes;
    Pane* active = nullptr;
    std::wstring notice;
    int workInset = 0;
    bool closing = false;

    void rebuildMenu()
    {
        HMENU menu = GetMenu(frame);
        while (GetMenuItemCount(menu) > 0) {
            if (!DeleteMenu(menu, 0, MF_BYPOSITION)) return;
        }
        auto add = [&](UINT command, const wchar_t* label) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE;
            item.fType = MFT_OWNERDRAW;
            item.wID = command;
            item.dwTypeData = const_cast<wchar_t*>(label);
            InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
        };
        add(openSettings, settingsMenu);
        if (manager.config.tabbed || panes.size() > 1) {
            for (size_t i = 0; i < panes.size(); ++i)
                add(firstTab + static_cast<UINT>(i), panes[i]->label.c_str());
        }
        DrawMenuBar(frame);
        PostMessageW(frame, updateLayout, 0, 0);
    }

    Pane* tab(UINT command) const
    {
        const auto index = command - firstTab;
        return command >= firstTab && index < panes.size() ? panes[index].get() : nullptr;
    }

    void applyTheme()
    {
        theme.readSystem();
        theme.setCaption(frame);
        MENUINFO info{sizeof(info)};
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = theme.menu();
        SetMenuInfo(GetMenu(frame), &info);
        DrawMenuBar(frame);
        RedrawWindow(frame, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_NOCHILDREN);
        for (const auto& pane : panes)
            RedrawWindow(pane->page, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
    }

    void title(const std::wstring& detail = {}) const
    {
        const std::wstring caption = detail.empty() ? L"WindowHost" : L"WindowHost — " + detail;
        SetWindowTextW(frame, caption.c_str());
    }

    int inset(const Pane* pane) const
    {
        return pane && manager.config.taskbarFor(pane->candidate.name, pane->slot) ? workInset : 0;
    }

    void layoutViewport() const
    {
        for (const auto& pane : panes)
            pane->resize({clientWidth, clientHeight - inset(pane.get())});
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
        if (GetMonitorInfoW(MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &monitor))
            inset = std::clamp<int>(monitor.rcMonitor.bottom - monitor.rcWork.bottom, 0, clientHeight - 1);
        if (inset != workInset) {
            workInset = inset;
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
        if (closing)
            return;
        DialogState state{*this, manager.config};
        if (active) {
            state.name = active->candidate.name;
            state.slot = active->slot;
            state.label = active->label;
        }
        const auto result = DialogBoxParamW(manager.instance, MAKEINTRESOURCEW(IDD_SETTINGS), frame,
                                            settingsProc, reinterpret_cast<LPARAM>(&state));
        if (result == IDOK)
            manager.apply(std::move(state.chosen), *this);
        else if (result == -1)
            MessageBoxW(frame, L"无法打开设置窗口。", L"WindowHost", MB_OK | MB_ICONERROR);
    }

    void select(Pane* next, bool focus)
    {
        if (active && active != next)
            active->show(false);
        active = next;
        if (active) {
            active->resize({clientWidth, clientHeight - inset(active)});
            active->show(true);
        }
        title(active ? active->label : std::wstring{});
        DrawMenuBar(frame);
        InvalidateRect(frame, nullptr, TRUE);
        if (focus)
            PostMessageW(frame, focusTarget, 0, 0);
    }

    DWORD start(const target::Candidate& candidate)
    {
        auto pane = std::make_unique<Pane>(candidate, manager.nextSlot(candidate.name));
        const DWORD error = pane->start(frame, theme, {clientWidth, clientHeight - inset(pane.get())});
        if (error)
            return error;
        panes.push_back(std::move(pane));
        rebuildMenu();
        if (!active)
            select(panes.back().get(), false);
        return ERROR_SUCCESS;
    }

    bool transfer(Pane* pane, Host& destination)
    {
        auto it = std::find_if(panes.begin(), panes.end(), [pane](const auto& p) { return p.get() == pane; });
        if (it == panes.end())
            return false;
        pane->show(false);
        if (!pane->moveTo(destination.frame, destination.theme)) {
            if (active == pane) pane->show(true);
            return false;
        }
        destination.panes.push_back(std::move(*it));
        panes.erase(it);
        if (active == pane) {
            active = nullptr;
            select(panes.empty() ? nullptr : panes.front().get(), false);
        }
        if (!destination.active)
            destination.select(pane, false);
        return true;
    }

    void finish(Pane* pane, DWORD error)
    {
        const auto it = std::find_if(panes.begin(), panes.end(), [pane](const auto& p) { return p.get() == pane; });
        if (it == panes.end())
            return;
        pane->join();
        if (error && error != ERROR_INVALID_WINDOW_HANDLE && error != ERROR_CANCELLED) {
            notice = pane->candidate.name + L"  " + captureError(error);
            manager.retries[pane->candidate.pid] = GetTickCount64() + 3000;
        }
        const size_t index = static_cast<size_t>(it - panes.begin());
        const bool selected = active == pane;
        if (selected) active = nullptr;
        panes.erase(it);
        if (panes.empty()) {
            const bool ownsDialog = manager.dialog && GetWindow(manager.dialog, GW_OWNER) == frame;
            if (closing || (notice.empty() && !ownsDialog && manager.hasOther(frame, false) &&
                            (!manager.config.tabbed || manager.group() != this))) {
                DestroyWindow(frame);
                return;
            }
        }
        rebuildMenu();
        if (selected)
            select(panes.empty() ? nullptr : panes[std::min(index, panes.size() - 1)].get(), !closing);
    }

    void close()
    {
        if (closing)
            return;
        closing = true;
        KillTimer(frame, 1);
        if (panes.empty()) {
            DestroyWindow(frame);
            return;
        }
        for (const auto& pane : panes) {
            manager.ignore(pane->candidate.pid);
            pane->release();
        }
        title(L"正在释放目标窗口，若停在断点请继续运行");
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
        const int bottomInset = inset(active);
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
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
        state = reinterpret_cast<DialogState*>(lp);
    if (!state)
        return FALSE;
    Host* app = &state->owner;
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        app->theme.setCaption(dialog);
        app->manager.dialog = dialog;
        std::wstring names;
        for (const auto& name : app->manager.config.names)
            names += name + L"\r\n";
        SendDlgItemMessageW(dialog, IDC_PROCESS_NAMES, EM_SETLIMITTEXT, 32767, 0);
        SetDlgItemTextW(dialog, IDC_PROCESS_NAMES, names.c_str());
        CheckRadioButton(dialog, IDC_LAYOUT_TABS, IDC_LAYOUT_WINDOWS,
                         state->chosen.tabbed ? IDC_LAYOUT_TABS : IDC_LAYOUT_WINDOWS);
        EnableWindow(GetDlgItem(dialog, IDC_TASKBAR), state->slot != 0);
        if (state->slot) {
            CheckDlgButton(dialog, IDC_TASKBAR, state->chosen.taskbarFor(state->name, state->slot) ? BST_CHECKED : BST_UNCHECKED);
            SetWindowTextW(dialog, (L"设置 — " + state->label).c_str());
        }
        return TRUE;
    }
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
            auto& chosen = state->chosen;
            chosen.tabbed = IsDlgButtonChecked(dialog, IDC_LAYOUT_TABS) == BST_CHECKED;
            if (state->slot)
                chosen.setTaskbar(state->name, state->slot, IsDlgButtonChecked(dialog, IDC_TASKBAR) == BST_CHECKED);
            auto error = settings::parseNames(lines, chosen);
            if (error.empty())
                error = settings::save(chosen);
            if (error.empty()) {
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
        else if (!lp && !app->closing) {
            if (auto* tab = app->tab(LOWORD(wp)))
                app->select(tab, true);
        }
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
        if (item.CtlType == ODT_MENU) {
            Pane* tab = app->tab(item.itemID);
            if (tab || item.itemID == openSettings) {
                measureHostMenu(window, item, tab ? tab->label : L"", !tab);
                return TRUE;
            }
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_MENU) {
            Pane* tab = app->tab(item.itemID);
            if (tab || item.itemID == openSettings) {
                drawHostMenu(window, item, tab ? tab->label : L"", !tab, tab && app->active == tab, app->theme);
                return TRUE;
            }
        }
        break;
    }
    case WM_MENUCHAR:
        if (reinterpret_cast<HMENU>(lp) == GetMenu(window)) {
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
    case target::restoring:
    case target::finished: {
        auto it = std::find_if(app->panes.begin(), app->panes.end(), [lp](const auto& pane) {
            return reinterpret_cast<LPARAM>(pane.get()) == lp;
        });
        if (it == app->panes.end())
            return 0;
        Pane* pane = it->get();
        if (message == target::finished)
            app->finish(pane, static_cast<DWORD>(wp));
        else if (message == target::restoring) {
            pane->releasing = true;
            if (pane == app->active) app->title(L"正在等待目标释放窗口");
        } else {
            pane->ready = true;
            if (pane == app->active && GetFocus() == window)
                PostMessageW(window, focusTarget, 0, 0);
        }
        return 0;
    }
    case WM_SETFOCUS:
        PostMessageW(window, focusTarget, 0, 0);
        break;
    case focusTarget:
        if (app->active && !app->closing && !app->manager.dialog && GetForegroundWindow() == window)
            app->active->focus();
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

Host* Manager::group() const
{
    Host* first = nullptr;
    for (const auto& host : hosts) {
        if (!host->frame || host->closing) continue;
        if (host->frame == groupFrame) return host.get();
        if (!first) first = host.get();
    }
    return first;
}

unsigned Manager::nextSlot(const std::wstring& name) const
{
    std::vector<unsigned> occupied;
    for (const auto& host : hosts)
        for (const auto& pane : host->panes)
            if (CompareStringOrdinal(name.c_str(), -1, pane->candidate.name.c_str(), -1, TRUE) == CSTR_EQUAL)
                occupied.push_back(pane->slot);
    std::sort(occupied.begin(), occupied.end());
    unsigned slot = 1;
    for (unsigned used : occupied) {
        if (slot != used) break;
        ++slot;
    }
    return slot;
}

bool Manager::arrange(Host& primary)
{
    // Creating split windows may grow hosts. Only visit the original windows.
    const size_t count = hosts.size();
    bool complete = true;
    for (size_t i = 0; i < count; ++i) {
        Host& source = *hosts[i];
        if (!source.frame || source.closing) continue;
        if (config.tabbed && &source != &primary) {
            while (!source.panes.empty()) {
                if (!source.transfer(source.panes.front().get(), primary)) {
                    complete = false;
                    break;
                }
            }
            if (source.panes.empty())
                DestroyWindow(source.frame);
        } else if (!config.tabbed) {
            while (source.panes.size() > 1) {
                Pane* moving = source.panes.back().get();
                if (moving == source.active) moving = source.panes.front().get();
                Host* dest = create(SW_HIDE, source.frame);
                if (!dest || !source.transfer(moving, *dest)) {
                    if (dest) DestroyWindow(dest->frame);
                    complete = false;
                    break;
                }
                ShowWindow(dest->frame, SW_SHOWNOACTIVATE);
            }
        }
    }
    for (const auto& host : hosts) {
        if (!host->frame || host->closing) continue;
        host->updateInset();
        host->layoutViewport();
        host->rebuildMenu();
        InvalidateRect(host->frame, nullptr, TRUE);
    }
    return complete;
}

void Manager::apply(settings::Config chosen, Host& origin)
{
    const bool layoutChanged = config.tabbed != chosen.tabbed;
    config = std::move(chosen);
    previous.clear();
    retries.clear();
    nextScan = 0;
    if (layoutChanged || pendingLayout) {
        groupFrame = origin.frame;
        pendingLayout = !arrange(origin);
        if (pendingLayout)
            MessageBoxW(origin.frame, L"部分窗口暂时无法移动，WindowHost 将自动重试。", L"WindowHost", MB_OK | MB_ICONWARNING);
    }
    for (const auto& host : hosts) {
        if (!host->frame || host->closing) continue;
        host->notice.clear();
        host->updateInset();
        host->layoutViewport();
        InvalidateRect(host->frame, nullptr, TRUE);
        for (const auto& pane : host->panes) {
            if (!config.watches(pane->candidate.name))
                pane->release();
        }
    }
}

Host* Manager::create(int show, HWND targetWindow)
{
    auto host = std::make_unique<Host>(*this);
    HMENU menu = CreateMenu();
    if (!menu)
        return nullptr;
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
    app->rebuildMenu();
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
    if (pendingLayout) {
        if (auto* root = group()) pendingLayout = !arrange(*root);
        if (pendingLayout) nextScan = now + 3000;
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
            bound = std::any_of(host->panes.begin(), host->panes.end(), [&](const auto& pane) {
                return pane->candidate.pid == candidate.pid;
            });
            if (bound) break;
            if (!idle && host->frame && !host->closing && host->panes.empty())
                idle = host.get();
        }
        if (bound)
            continue;
        Host* home = config.tabbed ? group() : idle;
        if (!home)
            home = create(SW_SHOWNOACTIVATE, candidate.window);
        if (!home) {
            retries[candidate.pid] = now + 3000;
            if (auto* root = group()) root->title(L"无法为 " + candidate.name + L" 创建窗口");
            continue;
        }
        const DWORD error = home->start(candidate);
        if (error) {
            retries[candidate.pid] = now + 3000;
            home->notice = candidate.name + L"  " + captureError(error);
            home->title(home->notice);
            InvalidateRect(home->frame, nullptr, TRUE);
        } else {
            home->notice.clear();
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
    if (!Pane::registerClass(instance))
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
