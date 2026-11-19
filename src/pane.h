#pragma once

#include "appearance.h"
#include "target.h"
#include <thread>

// A page is always Per-Monitor V2. Its inner viewport matches the target's
// DPI context. Moving the page never reparents the foreign window itself.
struct Pane {
    Pane(const target::Candidate& id, unsigned instanceSlot);
    ~Pane();
    static bool registerClass(HINSTANCE instance);
    DWORD start(HWND frame, Appearance& theme, SIZE size);
    bool moveTo(HWND frame, Appearance& theme);
    void resize(SIZE size) const;
    void show(bool visible);
    void focus() const;
    void release();
    void join();

    target::Candidate candidate;
    unsigned slot;
    std::wstring label;
    bool ready = false;
    bool releasing = false;
    HWND page = nullptr;
private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp);
    Appearance* colors = nullptr;
    HWND viewport = nullptr;
    HWND lastFocus = nullptr;
    HANDLE stop = nullptr;
    std::thread worker;
};
