#include "appearance.h"

#include <dwmapi.h>
#include <algorithm>
#include <cwchar>
#include <iterator>

namespace {
constexpr COLORREF canvas = RGB(28, 30, 34);
constexpr COLORREF input = RGB(49, 53, 60);
constexpr COLORREF bar = RGB(37, 40, 46);
constexpr COLORREF ink = RGB(237, 240, 245);

HFONT menuFont(HWND window)
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(window)))
        return nullptr;
    return CreateFontIndirectW(&metrics.lfMenuFont);
}
}

Appearance::Appearance() : darkBackground(CreateSolidBrush(canvas)), darkField(CreateSolidBrush(input)),
                           darkMenu(CreateSolidBrush(bar))
{
    readSystem();
}

Appearance::~Appearance()
{
    DeleteObject(darkBackground);
    DeleteObject(darkField);
    DeleteObject(darkMenu);
}

void Appearance::readSystem()
{
    HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    DWORD light = 1, bytes = sizeof(light);
    dark = !highContrast &&
           RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes) == ERROR_SUCCESS && light == 0;
}

void Appearance::setCaption(HWND window) const
{
    const BOOL enabled = dark;
    // Older Windows 10 builds used attribute 19 for the immersive title bar.
    if (FAILED(DwmSetWindowAttribute(window, 20, &enabled, sizeof(enabled))))
        DwmSetWindowAttribute(window, 19, &enabled, sizeof(enabled));
}

HBRUSH Appearance::background() const { return dark ? darkBackground : GetSysColorBrush(COLOR_WINDOW); }
HBRUSH Appearance::field() const { return dark ? darkField : GetSysColorBrush(COLOR_WINDOW); }
HBRUSH Appearance::menu() const { return dark ? darkMenu : GetSysColorBrush(COLOR_MENU); }
COLORREF Appearance::text() const { return dark ? ink : GetSysColor(COLOR_WINDOWTEXT); }
COLORREF Appearance::fieldColor() const { return dark ? input : GetSysColor(COLOR_WINDOW); }
COLORREF Appearance::secondary() const { return dark ? RGB(179, 187, 199) : GetSysColor(COLOR_WINDOWTEXT); }

void Appearance::drawButton(const DRAWITEMSTRUCT& item) const
{
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    RECT rect = item.rcItem;
    const int saved = SaveDC(item.hDC);
    FillRect(item.hDC, &rect, dark ? (pressed ? darkMenu : darkField) : GetSysColorBrush(COLOR_BTNFACE));
    FrameRect(item.hDC, &rect, dark ? GetSysColorBrush(COLOR_GRAYTEXT) : GetSysColorBrush(COLOR_BTNSHADOW));
    const auto font = SendMessageW(item.hwndItem, WM_GETFONT, 0, 0);
    if (font)
        SelectObject(item.hDC, reinterpret_cast<HFONT>(font));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, dark ? ink : GetSysColor(COLOR_BTNTEXT));
    wchar_t label[32]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    DrawTextW(item.hDC, label, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&rect, -3, -3);
        DrawFocusRect(item.hDC, &rect);
    }
    RestoreDC(item.hDC, saved);
}

void measureHostMenu(HWND frame, MEASUREITEMSTRUCT& item, const std::wstring& label, bool gear)
{
    const UINT dpi = GetDpiForWindow(frame);
    item.itemHeight = std::max(GetSystemMetricsForDpi(SM_CYMENU, dpi), MulDiv(26, dpi, 96));
    item.itemWidth = MulDiv(30, dpi, 96);
    if (gear) return;
    HDC dc = GetDC(frame);
    HFONT font = menuFont(frame);
    const auto old = font ? SelectObject(dc, font) : nullptr;
    RECT bounds{};
    DrawTextW(dc, label.c_str(), -1, &bounds, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    item.itemWidth = std::min<LONG>(bounds.right, MulDiv(260, dpi, 96)) + MulDiv(18, dpi, 96);
    if (font) {
        SelectObject(dc, old);
        DeleteObject(font);
    }
    ReleaseDC(frame, dc);
}

void drawHostMenu(HWND frame, const DRAWITEMSTRUCT& item, const std::wstring& label,
                  bool gear, bool selected, const Appearance& theme)
{
    const UINT dpi = GetDpiForWindow(frame);
    RECT rect = item.rcItem;
    const bool hot = (item.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    HBRUSH background = selected ? theme.background() : theme.menu();
    FillRect(item.hDC, &rect, hot ? GetSysColorBrush(COLOR_HIGHLIGHT) : background);
    const int saved = SaveDC(item.hDC);
    HFONT font = gear ? CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, L"Segoe MDL2 Assets") : menuFont(frame);
    if (font) SelectObject(item.hDC, font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, hot ? GetSysColor(COLOR_HIGHLIGHTTEXT) : theme.text());
    constexpr UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    if (gear) {
        DrawTextW(item.hDC, L"\uE713", 1, &rect, flags | DT_CENTER);
    } else {
        if (selected) {
            RECT line{rect.left, rect.bottom - MulDiv(2, dpi, 96), rect.right, rect.bottom};
            FillRect(item.hDC, &line, GetSysColorBrush(hot ? COLOR_HIGHLIGHTTEXT : COLOR_HIGHLIGHT));
        }
        InflateRect(&rect, -MulDiv(8, dpi, 96), 0);
        // Preserve the PID when the executable name needs an ellipsis.
        const auto suffix = label.rfind(L"  [");
        if (suffix != label.npos) {
            SIZE size{};
            GetTextExtentPoint32W(item.hDC, label.c_str() + suffix, static_cast<int>(label.size() - suffix), &size);
            DrawTextW(item.hDC, label.c_str() + suffix, -1, &rect, flags | DT_RIGHT);
            rect.right -= size.cx;
        }
        DrawTextW(item.hDC, label.c_str(), suffix == label.npos ? -1 : static_cast<int>(suffix),
                  &rect, flags | DT_END_ELLIPSIS);
    }
    RestoreDC(item.hDC, saved);
    if (font) DeleteObject(font);
}
