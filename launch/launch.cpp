// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  FA2Entropy (launcher) - minimal launcher, equivalent to the two lines in RunFA2sp.bat:
//
//      PUSHD %~dp0
//      START /B SYRINGE.EXE "FA2Entropy.dat" %*
//
//  Why a custom launcher (the original FA2SPLaunch.exe has no public source):
//    - the working directory is set to the launcher's own directory, so Syringe
//      and the editor resolve relative paths correctly
//    - command line arguments are forwarded (drag a .map file onto the icon to open it)
//    - the icon is compiled in by us (launch.rc -> legacyicon.ico)
//
//  Syringe is a console program; it is started with CREATE_NO_WINDOW so no console
//  window appears. Its output still goes to syringe.log.
// =============================================================================

#include <windows.h>
#include <stdio.h>   // swprintf_s

static void ShowError(const wchar_t* text)
{
    MessageBoxW(nullptr, text, L"FA2SPLaunch", MB_OK | MB_ICONERROR);
}

// Returns the part after the first argument (skips the exe itself, handles quoted paths)
static const wchar_t* ArgsAfterExe(const wchar_t* cmd)
{
    if (cmd == nullptr)
        return L"";

    const wchar_t* p = cmd;
    while (*p == L' ' || *p == L'\t') ++p;

    if (*p == L'"')
    {
        ++p;
        while (*p != 0 && *p != L'"') ++p;
        if (*p == L'"') ++p;
    }
    else
    {
        while (*p != 0 && *p != L' ' && *p != L'\t') ++p;
    }

    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    wchar_t dir[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, dir, MAX_PATH) == 0)
    {
        ShowError(L"取不到自己的路径");
        return 1;
    }

    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash != nullptr)
        *(slash + 1) = 0;

    SetCurrentDirectoryW(dir);   // = PUSHD %~dp0 in RunFA2sp.bat

    wchar_t syringe[MAX_PATH] = { 0 };
    wcscpy_s(syringe, dir);
    wcscat_s(syringe, L"SYRINGE.EXE");

    if (GetFileAttributesW(syringe) == INVALID_FILE_ATTRIBUTES)
    {
        ShowError(L"找不到 SYRINGE.EXE —— 请把本程序放在地图编辑器目录里");
        return 1;
    }

    wchar_t target[MAX_PATH] = { 0 };
    wcscpy_s(target, dir);
    wcscat_s(target, L"FA2Entropy.dat");

    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES)
    {
        ShowError(L"找不到 FA2Entropy.dat —— 请把本程序放在地图编辑器目录里");
        return 1;
    }

    // Command line = "<full path to SYRINGE.EXE>" "FA2Entropy.dat" <caller arguments>
    wchar_t cmd[4096] = { 0 };
    swprintf_s(cmd, L"\"%s\" \"FA2Entropy.dat\" %s",
               syringe, ArgsAfterExe(GetCommandLineW()));

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi = { 0 };

    if (CreateProcessW(syringe, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, dir, &si, &pi) == FALSE)
    {
        wchar_t msg[256] = { 0 };
        swprintf_s(msg, L"启动 SYRINGE.EXE 失败（错误码 %lu）", GetLastError());
        ShowError(msg);
        return 1;
    }

    // If it exits immediately, injection failed: report the exit code, which is
    // easier to debug than a window that never appears.
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);

        if (code != 0)
        {
            wchar_t msg[256] = { 0 };
            swprintf_s(msg, L"SYRINGE.EXE 很快退出了（退出码 %lu）\n\n"
                            L"看地编目录里的 syringe.log 和 FA2Entropy.log。", code);
            ShowError(msg);
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
