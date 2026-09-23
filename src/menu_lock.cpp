// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  "Options" menu watchdog
//
//  · Deletes by exact text match (after removing & mnemonics and the \t shortcut,
//    the whole string must be equal), so items like "Paste Options" are not hit
//  · Re-checks every 1.5 s in case the editor rebuilds its menu
//  · Debug mode: a FA2Entropy.logonly file next to the DLL dumps the whole menu
//    tree to the log and deletes nothing
//
//  Which items to delete comes from the [Menu] section of assets\FA2Entropy.ini
// =============================================================================

#include "menu_lock.h"
#include "lock_config.h"
#include "entropy_cfg.h"
#include "icon_fix.h"
#include "log.h"

#include <windows.h>
#include <stdlib.h>

#include <string>
#include <vector>

namespace qecmenu {

// Items to kill: command IDs (primary), texts (fallback), dialog titles, interval — see [Menu]
static std::vector<unsigned>     g_killIds;
static std::vector<std::wstring> g_killWords;
static std::vector<std::wstring> g_dialogTitles;
static bool g_killTopLevel = false;
static int  g_interval = 700;

static DWORD   g_pid = 0;
static HMODULE g_self = nullptr;
static HANDLE  g_thread = nullptr;
static volatile LONG g_stop = 0;

static std::wstring Lower(const std::wstring& s)
{
    std::wstring r = s;
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = static_cast<wchar_t>(towlower(r[i]));
    return r;
}

// Strip mnemonics (&) and any "\tshortcut" suffix
static std::wstring CleanMenuText(const std::wstring& s)
{
    const size_t tab = s.find(L'\t');
    const std::wstring t = (tab == std::wstring::npos) ? s : s.substr(0, tab);

    std::wstring r;
    for (size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] != L'&')
            r += t[i];
    }
    return r;
}

// Exact match (not substring): command ID hit, or cleaned text fully equal
static bool ShouldKill(HMENU menu, int pos, const std::wstring& text)
{
    const unsigned id = static_cast<unsigned>(GetMenuItemID(menu, pos));

    for (unsigned want : g_killIds)
    {
        if (id == want)
            return true;
    }

    const std::wstring clean = Lower(CleanMenuText(text));
    for (const std::wstring& w : g_killWords)
    {
        if (clean == Lower(w))
            return true;
    }

    return false;
}

// Top-level "Options" menu / "Options" dialog title (same word list)
static bool IsOptionsWord(const std::wstring& text)
{
    const std::wstring clean = Lower(CleanMenuText(text));

    for (const std::wstring& w : g_dialogTitles)
    {
        if (clean == Lower(w))
            return true;
    }

    return false;
}

static bool IsOptionsDialogTitle(const std::wstring& title)
{
    const std::wstring clean = Lower(title);

    for (const std::wstring& w : g_dialogTitles)
    {
        if (clean == Lower(w))
            return true;
    }

    return false;
}

// Is there a FA2Entropy.logonly file next to the DLL? If yes, log only, never delete
static bool LogOnlyMode()
{
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;

    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(g_self, path, MAX_PATH);

    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash != nullptr)
        *(slash + 1) = 0;
    else
        path[0] = 0;

    wcscat_s(path, ENTROPY_LOGONLY_MARKER);
    cached = (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    return cached == 1;
}

static void DumpMenu(HMENU menu, const std::wstring& prefix)
{
    const int count = GetMenuItemCount(menu);

    for (int i = 0; i < count; ++i)
    {
        wchar_t buf[256] = { 0 };
        GetMenuStringW(menu, i, buf, 256, MF_BYPOSITION);

        const std::wstring raw(buf);
        const std::wstring path = prefix.empty() ? raw : (prefix + L" → " + raw);

        if (HMENU sub = GetSubMenu(menu, i))
        {
            qeclog::Write(L"[menu]   %s  (子菜单)", path.c_str());
            DumpMenu(sub, path);
        }
        else
        {
            const UINT id = GetMenuItemID(menu, i);
            qeclog::Write(L"[menu]   %s  (命令 ID=%u，净文字=\"%s\")",
                          path.c_str(), id, CleanMenuText(raw).c_str());
        }
    }
}

static int KillInMenu(HMENU menu, const std::wstring& prefix, bool logOnly)
{
    int killed = 0;
    const int count = GetMenuItemCount(menu);

    // Delete backwards so positions do not shift
    for (int i = count - 1; i >= 0; --i)
    {
        wchar_t buf[256] = { 0 };
        GetMenuStringW(menu, i, buf, 256, MF_BYPOSITION);

        const std::wstring raw(buf);
        const std::wstring path = prefix.empty() ? raw : (prefix + L" → " + raw);

        if (HMENU sub = GetSubMenu(menu, i))
            killed += KillInMenu(sub, path, logOnly);

        bool kill = ShouldKill(menu, i, raw);

        // With KillTopLevelOptions=1, drop the whole top-level "Options" menu too
        if (g_killTopLevel && prefix.empty() && IsOptionsWord(raw))
            kill = true;

        if (kill)
        {
            if (logOnly)
            {
                qeclog::Write(L"[menu] [仅记录] 命中要删的项：%s", path.c_str());
                continue;
            }

            if (DeleteMenu(menu, i, MF_BYPOSITION))
            {
                qeclog::Write(L"[menu] 已删除菜单项：%s", path.c_str());
                killed++;
            }
        }
    }

    return killed;
}

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_pid)
        return TRUE;

    // ---- Replace the window icon first (title bar + taskbar, once per window) ----
    qecicon::ApplyOnce(hwnd);

    // ---- Then handle the "Options" dialog: close it as soon as it appears ----
    if (IsWindowVisible(hwnd))
    {
        wchar_t title[256] = { 0 };
        GetWindowTextW(hwnd, title, 256);

        if (title[0] != 0 && IsOptionsDialogTitle(std::wstring(title)))
        {
            if (LogOnlyMode())
            {
                qeclog::Write(L"[menu] [仅记录] 发现「%s」对话框（本该自动关闭）", title);
            }
            else
            {
                qeclog::Write(L"[menu] 发现「%s」对话框 → 自动关闭", title);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }

            return TRUE;
        }
    }

    HMENU menu = GetMenu(hwnd);
    if (menu == nullptr)
        return TRUE;

    const bool firstRound = (lParam != 0);
    const bool logOnly = LogOnlyMode();

    if (firstRound && logOnly)
    {
        qeclog::Write(L"[menu] 调试模式（发现 FA2Entropy.logonly）：只记录不删除，整棵菜单树如下");
        DumpMenu(menu, L"");
    }

    if (KillInMenu(menu, L"", logOnly) > 0)
        DrawMenuBar(hwnd);

    return TRUE;
}

static DWORD WINAPI WatchProc(LPVOID /*param*/)
{
    Sleep(1000); // let the main window appear

    bool announced = false;
    bool firstRound = true;

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
    {
        EnumWindows(EnumProc, firstRound ? 1 : 0);

        if (!announced)
        {
            announced = true;
            qeclog::Write(L"[menu] 看门狗运行中（每 %d ms 复查一次）%s",
                          g_interval,
                          LogOnlyMode() ? L"【调试：只记录不删除】" : L"");
        }

        firstRound = false;
        Sleep(g_interval);
    }

    return 0;
}

void Start(HMODULE self)
{
    g_self = self;

    // ---- Menu config (the [Menu] section of assets\FA2Entropy.ini) ----
    g_killIds.clear();
    for (const std::wstring& v : cfg::All(L"Menu", L"KillId"))
    {
        const unsigned id = static_cast<unsigned>(wcstoul(v.c_str(), nullptr, 0));
        if (id != 0)
            g_killIds.push_back(id);
    }

    g_killWords    = cfg::All(L"Menu", L"KillWord");
    g_dialogTitles = cfg::All(L"Menu", L"KillDialog");
    g_killTopLevel = cfg::B(L"Menu", L"KillTopLevelOptions", false);
    g_interval     = cfg::I(L"Menu", L"WatchInterval", 700);
    if (g_interval < 100)
        g_interval = 100;   // keep it from burning CPU

    qeclog::Write(L"[menu] 配置：删 %u 个命令 ID / %u 个文字 / 关 %u 个标题窗口，复查间隔 %d ms",
                  static_cast<unsigned>(g_killIds.size()),
                  static_cast<unsigned>(g_killWords.size()),
                  static_cast<unsigned>(g_dialogTitles.size()),
                  g_interval);

    g_pid = GetCurrentProcessId();
    g_thread = CreateThread(nullptr, 0, WatchProc, nullptr, 0, nullptr);

    if (g_thread != nullptr)
        qeclog::Write(L"[menu] 看门狗线程已创建（PID %lu）", g_pid);
    else
        qeclog::Write(L"[menu] !! 看门狗线程创建失败：%lu", GetLastError());
}

void Stop()
{
    InterlockedExchange(&g_stop, 1);

    if (g_thread != nullptr)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

} // namespace qecmenu
