// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

#include "log.h"
#include "lock_config.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace qeclog {

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

// Buffer lines until we know whether a log file is wanted: flush them if it opens, drop them if not
static bool g_decided = false;
static std::vector<std::string> g_pending;

static void WriteRaw(const char* data, int len)
{
    if (g_file == INVALID_HANDLE_VALUE || data == nullptr || len <= 0)
        return;

    DWORD written = 0;
    WriteFile(g_file, data, static_cast<DWORD>(len), &written, nullptr);
}

void Init(HMODULE /*self*/)
{
    if (g_csReady)
        return;

    InitializeCriticalSection(&g_cs);
    g_csReady = true;
}

// Open the log file (only reached when the config has Enabled=1)
static void OpenFile(HMODULE self)
{
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(self, path, MAX_PATH);

    // Swap the file name for the log name, in the DLL's own directory
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash != nullptr)
        *(slash + 1) = 0;
    else
        path[0] = 0;

    wcscat_s(path, ENTROPY_LOG_FILENAME);

    g_file = CreateFileW(path, FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (g_file == INVALID_HANDLE_VALUE)
        return;

    // New file: write a UTF-8 BOM so Notepad detects the encoding
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(g_file, &size) && size.QuadPart == 0)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        WriteRaw(reinterpret_cast<const char*>(bom), 3);
    }
}

void SetFileEnabled(HMODULE self, bool on)
{
    if (g_csReady)
        EnterCriticalSection(&g_cs);

    g_decided = true;

    if (on)
    {
        OpenFile(self);

        // Flush the lines buffered before the decision, for a complete record
        for (const std::string& line : g_pending)
            WriteRaw(line.data(), static_cast<int>(line.size()));
    }

    g_pending.clear();

    if (g_csReady)
        LeaveCriticalSection(&g_cs);
}

void Write(const wchar_t* fmt, ...)
{
    // [Log] Enabled=0: no log file, no debugger output, nothing at all
    if (g_decided && g_file == INVALID_HANDLE_VALUE)
        return;

    wchar_t wbuf[2048] = { 0 };

    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(wbuf, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char head[64] = { 0 };
    const int headLen = sprintf_s(head, "[%02d:%02d:%02d.%03d] ",
                                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    char ubuf[4096] = { 0 };
    int uLen = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf,
                                   static_cast<int>(sizeof(ubuf)) - 1, nullptr, nullptr);
    if (uLen > 0)
        uLen--; // drop the trailing NUL

    if (g_csReady)
        EnterCriticalSection(&g_cs);

    if (g_file != INVALID_HANDLE_VALUE)
    {
        WriteRaw(head, headLen);
        WriteRaw(ubuf, uLen);
        WriteRaw("\r\n", 2);
    }
    else if (!g_decided && g_pending.size() < 2000)
    {
        // Undecided: buffer the line with its timestamp so it can be written verbatim later
        std::string line(head, static_cast<size_t>(headLen));
        line.append(ubuf, static_cast<size_t>(uLen));
        line += "\r\n";
        g_pending.push_back(line);
    }

    if (g_csReady)
        LeaveCriticalSection(&g_cs);

    // Debugger output is part of logging, so it only happens while a log file is open
    if (g_file != INVALID_HANDLE_VALUE)
    {
        OutputDebugStringW(wbuf);
        OutputDebugStringW(L"\n");
    }
}

void Close()
{
    if (g_file != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

} // namespace qeclog
