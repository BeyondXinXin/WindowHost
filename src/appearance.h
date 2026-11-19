#pragma once

#include <windows.h>

struct Appearance {
    Appearance();
    ~Appearance();
    Appearance(const Appearance&) = delete;
    Appearance& operator=(const Appearance&) = delete;

    void readSystem();
    void setCaption(HWND window) const;
    HBRUSH background() const;
    HBRUSH field() const;
    HBRUSH menu() const;
    COLORREF text() const;
    COLORREF fieldColor() const;
    COLORREF secondary() const;
    void drawButton(const DRAWITEMSTRUCT& item) const;

    bool dark = false;
private:
    HBRUSH darkBackground;
    HBRUSH darkField;
    HBRUSH darkMenu;
};

// Owner-drawn menu items are used only in dark mode. Standard menus handle
// their own light and high-contrast colors, metrics and keyboard cues.
void measureDarkMenu(HWND frame, MEASUREITEMSTRUCT& item, const wchar_t* label);
void drawDarkMenu(HWND frame, const DRAWITEMSTRUCT& item, const wchar_t* label, const Appearance& theme);
