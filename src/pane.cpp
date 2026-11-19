#include "pane.h"
#include <system_error>

namespace {
constexpr wchar_t paneClass[] = L"BeyondXinXin.WindowHost.Page";
constexpr DWORD childStyle = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
}

Pane::Pane(const target::Candidate& id, unsigned instanceSlot)
    : candidate(id), slot(instanceSlot), label(id.name + L"  [" + std::to_wstring(id.pid) + L"]") {}

Pane::~Pane()
{
    // The UI removes a running page only after receiving the worker's finished message.
    if (page) DestroyWindow(page);
    if (stop) CloseHandle(stop);
}

bool Pane::registerClass(HINSTANCE instance)
{
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpfnWndProc = windowProc;
    cls.lpszClassName = paneClass;
    return RegisterClassW(&cls) != 0;
}

DWORD Pane::start(HWND frame, Appearance& theme, SIZE size)
{
    colors = &theme;
    stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop)
        return GetLastError();
    const auto hosting = SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
    if (hosting == DPI_HOSTING_BEHAVIOR_INVALID)
        return ERROR_INVALID_STATE;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    page = CreateWindowExW(0, paneClass, nullptr, childStyle, 0, 0, size.cx, size.cy, frame, nullptr, instance, this);
    const DWORD pageError = page ? ERROR_SUCCESS : GetLastError();
    SetThreadDpiHostingBehavior(hosting);
    if (!page)
        return pageError ? pageError : ERROR_GEN_FAILURE;
    const auto context = GetWindowDpiAwarenessContext(candidate.window);
    const auto previous = SetThreadDpiAwarenessContext(context);
    if (!previous)
        return ERROR_INVALID_STATE;
    viewport = CreateWindowExW(0, paneClass, nullptr, childStyle | WS_VISIBLE, 0, 0, 0, 0, page, nullptr, instance, this);
    const DWORD viewError = viewport ? ERROR_SUCCESS : GetLastError();
    SetThreadDpiAwarenessContext(previous);
    if (!viewport)
        return viewError ? viewError : ERROR_GEN_FAILURE;
    if (!AreDpiAwarenessContextsEqual(context, GetWindowDpiAwarenessContext(viewport)))
        return ERROR_INVALID_STATE;
    resize(size);
    try {
        worker = std::thread([notify = page, view = viewport, id = candidate, cancel = stop] {
            const DWORD error = target::host(notify, view, id, cancel);
            PostMessageW(notify, target::finished, error, 0);
        });
    } catch (const std::system_error&) {
        return ERROR_NO_SYSTEM_RESOURCES;
    }
    return ERROR_SUCCESS;
}

bool Pane::moveTo(HWND frame, Appearance& theme)
{
    if (!SetParent(page, frame))
        return false;
    colors = &theme;
    return true;
}

void Pane::resize(SIZE size) const
{
    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(page, nullptr, 0, 0, size.cx, size.cy, flags);
    SetWindowPos(viewport, nullptr, 0, 0, size.cx, size.cy, flags);
}

void Pane::show(bool visible)
{
    if (!visible) {
        HWND focused = GetFocus();
        if (focused == candidate.window || IsChild(candidate.window, focused))
            lastFocus = focused;
    }
    ShowWindow(page, visible ? SW_SHOWNA : SW_HIDE);
}

void Pane::focus() const
{
    if (!ready || releasing || !IsChild(viewport, candidate.window) || !IsWindowVisible(candidate.window) ||
        !IsWindowEnabled(candidate.window) || IsHungAppWindow(candidate.window))
        return;
    HWND dest = lastFocus && IsChild(candidate.window, lastFocus) && IsWindowEnabled(lastFocus) &&
                IsWindowVisible(lastFocus) ? lastFocus : candidate.window;
    SetFocus(dest);
}

void Pane::release()
{
    releasing = true;
    SetEvent(stop);
}

void Pane::join() { worker.join(); }

LRESULT CALLBACK Pane::windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* pane = reinterpret_cast<Pane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (pane && message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wp), &rect, pane->colors->background());
        return 1;
    }
    if (pane && window == pane->page &&
        (message == target::finished || message == target::attached || message == target::restoring)) {
        // Routing through the current parent also covers a message queued before a layout change.
        return SendMessageW(GetParent(window), message, wp, reinterpret_cast<LPARAM>(pane));
    }
    return DefWindowProcW(window, message, wp, lp);
}
