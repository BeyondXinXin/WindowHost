#pragma once

#include <windows.h>
#include <filesystem>

namespace target {
struct Candidate {
    HWND window = nullptr;
    DWORD pid = 0;
    DWORD thread = 0;
};

Candidate findMainWindow(const std::filesystem::path& exe);

constexpr UINT attached = WM_APP + 1;
constexpr UINT restoring = WM_APP + 2;
constexpr UINT finished = WM_APP + 3;

// Run on a worker. The UI owns frame, viewport and the manual-reset stop event
// until this returns. A finished session leaves no foreign child in viewport.
DWORD host(HWND frame, HWND viewport, Candidate candidate,
           const std::filesystem::path& exe, HANDLE stop);
}
