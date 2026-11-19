#pragma once

#include <windows.h>
#include <string>

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

void measureHostMenu(HWND frame, MEASUREITEMSTRUCT& item, const std::wstring& label, bool gear);
void drawHostMenu(HWND frame, const DRAWITEMSTRUCT& item, const std::wstring& label,
                  bool gear, bool selected, const Appearance& theme);
