#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace target {
struct Candidate {
    HWND window = nullptr;
    DWORD pid = 0;
    DWORD thread = 0;
    std::wstring name;
};

std::vector<Candidate> findMainWindows(const std::vector<std::wstring>& names);

constexpr UINT attached = WM_APP + 1;
constexpr UINT restoring = WM_APP + 2;
constexpr UINT finished = WM_APP + 3;

// The notification window and viewport travel with their page and outlive the worker.
DWORD host(HWND notify, HWND viewport, const Candidate& candidate, HANDLE stop);
}
