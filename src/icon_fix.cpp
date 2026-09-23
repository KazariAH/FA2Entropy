// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

#include "icon_fix.h"
#include "lock_config.h"
#include "log.h"

#include <windows.h>

namespace qecicon {

static HMODULE g_self = nullptr;
static HWND    g_done = nullptr;

void Install(HMODULE self)
{
    g_self = self;
}

bool ApplyOnce(HWND hwnd)
{
    if (hwnd == nullptr || g_self == nullptr)
        return false;

    if (hwnd == g_done)
        return false;   // this window is already done

    if (IsWindowVisible(hwnd) == FALSE)
        return false;

    // Main window criterion: a non-empty title is enough (EnumWindows only yields top-level windows)
    wchar_t title[256] = { 0 };
    GetWindowTextW(hwnd, title, 256);
    if (title[0] == 0)
        return false;

    // Do not release these icon handles: the window keeps using them
    HICON icoBig = reinterpret_cast<HICON>(
        LoadImageW(g_self, MAKEINTRESOURCEW(RES_APP_ICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));

    HICON icoSm = reinterpret_cast<HICON>(
        LoadImageW(g_self, MAKEINTRESOURCEW(RES_APP_ICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

    if (icoBig == nullptr)
    {
        qeclog::Write(L"[icon] !! 取不到内置图标（RES_APP_ICON=%d）", RES_APP_ICON);
        return false;
    }

    if (icoSm == nullptr)
        icoSm = icoBig;

    // Title bar (top left)
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icoSm));
    // Taskbar / Alt-Tab (big icon)
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icoBig));

    // Replace the window class icon too, so the title bar / taskbar cannot fall back to the FA2sp one
    SetClassLongPtrW(hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(icoSm));
    SetClassLongPtrW(hwnd, GCLP_HICON,   reinterpret_cast<LONG_PTR>(icoBig));

    g_done = hwnd;

    qeclog::Write(L"[icon] 窗口图标已换成 FA2Entropy 的（标题栏 + 任务栏 ✓）");
    return true;
}

} // namespace qecicon
