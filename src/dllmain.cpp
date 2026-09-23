// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  FA2Entropy - configuration-injection DLL for the FinalAlert2 map editor.
//
//  Injected with Syringe (RunFA2sp.bat: SYRINGE.EXE "FA2Entropy.dat"), which scans
//  the target directory for *.dll and loads only those declaring hooks, so the DLL
//  carries a .syhks00 section instead of a separate .inj file. It compiles the QEC
//  adaptation configs into the DLL resources (assets\FinalAlert.ini and three other ini
//  files, see FA2Entropy.rc) and makes the editor read that shadow config, so the
//  released package needs no such file on disk. Change configuration by editing assets\
//  and rebuilding.
//
//  Syringe also loads this DLL into its own process to resolve hooks: install no hook
//  and start no thread there, or its unload crashes. Hence the host check below.
// =============================================================================

#include "log.h"
#include "ini_lock.h"
#include "menu_lock.h"
#include "file_redirect.h"
#include "lock_config.h"
#include "entropy_cfg.h"
#include "icon_fix.h"

#include <windows.h>

// Syringe resolves this name with GetProcAddress (the placeholder hook itself is unused,
// the export only keeps the log clean).
//
// Must stay __cdecl (the default): with WINAPI/stdcall the x86 export name becomes
// _QecBootstrap@4 and GetProcAddress("QecBootstrap") fails.
extern "C" __declspec(dllexport) void QecBootstrap(void* /*context*/)
{
}

static HMODULE g_self = nullptr;
static bool    g_active = false;

// File name of the current process main module (FA2Entropy.dat / Syringe.exe ...)
static const wchar_t* HostBaseName()
{
    static wchar_t path[MAX_PATH] = { 0 };
    if (path[0] != 0)
        return path;

    wchar_t full[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, full, MAX_PATH);

    const wchar_t* base = full;
    for (const wchar_t* p = full; *p != 0; ++p)
    {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }

    wcscpy_s(path, base);
    return path;
}

static bool IsExpectedHost()
{
    return _wcsicmp(HostBaseName(), cfg::HostExe().c_str()) == 0;
}

// Record the host timestamp (informational only, never blocks)
static void LogHostInfo()
{
    HMODULE host = GetModuleHandleW(nullptr);
    if (host == nullptr)
        return;

    DWORD stamp = 0;
    const BYTE* img = reinterpret_cast<const BYTE*>(host);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img);

    if (dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(img + dos->e_lfanew);

        if (nt->Signature == IMAGE_NT_SIGNATURE)
            stamp = nt->FileHeader.TimeDateStamp;
    }

    const unsigned want = cfg::HostTimeStamp();

    qeclog::Write(L"[host] 宿主 = %s，时间戳 = 0x%08X（期望 0x%08X%s）",
                  HostBaseName(), stamp, want,
                  (stamp == want) ? L" ✓" : L" ⚠ 不是预期版本（功能照常）");
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = self;
        DisableThreadLibraryCalls(self);

        qeclog::Init(self);

        // Read the config first (the four ini resources); every check below relies on it
        cfg::Load(self);

        // Log file switch: default 0 creates no FA2Entropy.log (logging still goes to OutputDebugString)
        qeclog::SetFileEnabled(self, cfg::B(L"Log", L"Enabled", false));

        // Not the editor: do nothing, since Syringe loads this DLL during handshake
        if (!IsExpectedHost())
        {
            qeclog::Write(L"[host] 当前进程是 %s（不是 %s）→ 不装 hook、不起线程，直接返回",
                          HostBaseName(), cfg::HostExe().c_str());
            return TRUE;
        }

        g_active = true;

        qeclog::Write(L"");
        qeclog::Write(L"================ FA2Entropy 已注入 ================");
        LogHostInfo();

        // Shadow config + file redirection: must be installed before the editor reads any ini
        qecfile::Install(self);

        // profile API read/write interception (language and FileSearchLikeTS use this path)
        if (qecini::Install())
            qeclog::Write(L"[ini] 安装完成 ✓");
        else
            qeclog::Write(L"[ini] !! 安装有失败项，注入可能不完整");

        // Window icon: set legacyicon on the editor window (title bar and taskbar)
        qecicon::Install(self);

        // Remove "Options -> Settings" and auto-close the "Options" dialog
        qecmenu::Start(self);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_active)
        {
            qecmenu::Stop();
            qecini::Uninstall();
            qecfile::Uninstall();   // erase the paths stored in the shadow file on exit
            qeclog::Write(L"================ FA2Entropy 卸载 ================");
        }

        qeclog::Close();
    }

    return TRUE;
}
