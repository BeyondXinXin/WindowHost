#include "appearance.h"

#include <dwmapi.h>
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

void measureDarkMenu(HWND frame, MEASUREITEMSTRUCT& item, const wchar_t* label)
{
    HDC dc = GetDC(frame);
    HFONT font = menuFont(frame);
    const auto old = font ? SelectObject(dc, font) : nullptr;
    RECT size{};
    DrawTextW(dc, label, -1, &size, DT_SINGLELINE | DT_CALCRECT);
    const UINT dpi = GetDpiForWindow(frame);
    item.itemWidth = size.right + MulDiv(12, dpi, 96);
    item.itemHeight = GetSystemMetricsForDpi(SM_CYMENU, dpi);
    if (font) {
        SelectObject(dc, old);
        DeleteObject(font);
    }
    ReleaseDC(frame, dc);
}

void drawDarkMenu(HWND frame, const DRAWITEMSTRUCT& item, const wchar_t* label, const Appearance& theme)
{
    const bool active = (item.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    FillRect(item.hDC, &item.rcItem, active ? theme.field() : theme.menu());
    const int saved = SaveDC(item.hDC);
    HFONT font = menuFont(frame);
    if (font)
        SelectObject(item.hDC, font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme.text());
    RECT rect = item.rcItem;
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
    if (item.itemState & ODS_NOACCEL)
        flags |= DT_HIDEPREFIX;
    DrawTextW(item.hDC, label, -1, &rect, flags);
    RestoreDC(item.hDC, saved);
    if (font)
        DeleteObject(font);
}
