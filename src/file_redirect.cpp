// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
// FinalAlert.ini shadow copy + CreateFileA/W redirection.
// The editor's own INI reader bypasses profile-API hooks, so opens of FinalAlert.ini and
// the include lists are served from a shadow copy in %LOCALAPPDATA%\FA2Entropy; delete it to undo.
// =============================================================================

#include "file_redirect.h"
#include "lock_config.h"
#include "entropy_cfg.h"
#include "ini_lock.h"
#include "log.h"

#include "MinHook.h"

#include <windows.h>
#include <string>
#include <vector>

namespace qecfile {

typedef HANDLE (WINAPI *PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *PFN_CreateFileA)(LPCSTR,  DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static PFN_CreateFileW g_CreateFileW = nullptr;
static PFN_CreateFileA g_CreateFileA = nullptr;

static std::wstring g_shadowPath;   // full path of the shadow ini
static std::wstring g_shadowDir;    // directory holding shadow files
static std::wstring g_editorDir;    // editor directory; only files in it are redirected

// ---- all lists below come from assets\FA2Entropy.ini, read once at startup ----

// Files whose include order must be forced ([Include] Fix=FAData.ini=qec01.dat)
struct FixEntry { std::wstring file; std::wstring entry; };
static std::vector<FixEntry> g_fixes;

// Files to block ([Include] Drop=xxx.ini)
static std::vector<std::wstring> g_drops;

// Shadow file name, shadow directory name, and locked ini ([Shadow] / [Lock])
static std::wstring g_shadowDirName;
static std::wstring g_shadowName;
static std::wstring g_lockedIni;

// Preference store ([Lock] UserIni=): holds the non-fixed keys of the locked ini.
// It lives in the editor directory on purpose and is therefore never moved or redirected.
static std::wstring g_userIni;
static std::wstring g_userIniPath;

// Target subfolder for ini/data files ([Paths] DataDir=), full path; empty = no move, no redirect
static std::wstring g_dataDir;

// Target subfolder for dlls ([Search] DllDir=), full path; empty = do not move
static std::wstring g_dllDir;

// Our own file name (FA2Entropy.dll); always skipped when moving dlls
static std::wstring g_selfName;

// Additional files redirected to a shadow copy
// blocked=true means blanked: redirect to an empty file so no data reaches the editor
struct RedirectEntry { std::wstring base; std::wstring to; bool blocked = false; };
static std::vector<RedirectEntry> g_extraRedirects;

// Files extracted from DLL resources; deleted on uninstall
static std::vector<std::wstring> g_extracted;

// Data files from [DataFiles] Move= (lowercase names); currently only Quantum.rpck
//   Only listed files are moved or redirected; nothing else is touched
static std::vector<std::string> g_dataFileLows;

// [Others] list and its target folder for extra files
static std::vector<std::string> g_otherFileLows;
static std::wstring g_othersDir;

// Forward declarations: ExtractAsset needs WriteAllBytes, EnsureIncludeFix needs IniSourcePath
static bool WriteAllBytes(const std::wstring& p, const std::string& data);
static std::wstring IniSourcePath(const std::wstring& name);
static bool FileExists(const std::wstring& p);
static std::string ReadAllBytes(const std::wstring& p);
static void ClearAttributes(const std::wstring& path);

// ---------------------------------------------------------------------------
// Working copies
//
//  The editor's data files (qec01.dat / qec02.dat released from the DLL resources, and
//  the rebuilt FAData.ini / FALanguage.ini) are built on every start and thrown away on
//  shutdown, so that no modified data file stays on disk. Preferences the editor writes
//  into them would be lost with them, so:
//
//    start : the preference store is merged into every copy before it is written
//    exit  : the copy is compared with what we wrote, and the changed keys are saved
//            back into the preference store
//
//  Which file the editor uses for its settings therefore does not matter.
// ---------------------------------------------------------------------------
struct ManagedCopy
{
    std::wstring path;
    std::string  baseline;   // exact bytes we wrote at start-up
};

static std::vector<ManagedCopy> g_managed;

static void RememberCopy(const std::wstring& path, const std::string& content)
{
    ManagedCopy c;
    c.path = path;
    c.baseline = content;
    g_managed.push_back(c);

    qeclog::Write(L"[file] 工作副本（退出时会比对并保存改动）：%s（%u 字节）",
                  path.c_str(), static_cast<unsigned>(content.size()));
}

// Merge the preference store into a working copy (ANSI text) before it is written
static void OverlayStore(std::string& text)
{
    if (g_userIniPath.empty() || !FileExists(g_userIniPath))
        return;

    const std::string store = ReadAllBytes(g_userIniPath);
    if (store.empty())
        return;

    text = qecini::MergeTextIntoIniText(text, store);
}

// Extract a DLL resource (Quantum01/02.ini) into the shadow dir and register its redirect
static bool ExtractAsset(HMODULE self, int resId, const wchar_t* outName)
{
    const HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (res == nullptr)
    {
        qeclog::Write(L"[file] !! 找不到资源 %d（编译时 assets\\%s 缺失？）", resId, outName);
        return false;
    }

    const HGLOBAL h = LoadResource(self, res);
    const void* data = (h != nullptr) ? LockResource(h) : nullptr;
    const DWORD size = (h != nullptr) ? SizeofResource(self, res) : 0;

    if (data == nullptr || size == 0)
    {
        qeclog::Write(L"[file] !! 资源 %d 读不出来", resId);
        return false;
    }

    std::string bytes(reinterpret_cast<const char*>(data), size);
    const std::wstring dst = g_shadowDir + L"\\" + outName;

    // Settings from earlier runs are merged back in before the copy is written
    OverlayStore(bytes);

    if (!WriteAllBytes(dst, bytes))
    {
        qeclog::Write(L"[file] !! 释放 %s 失败", outName);
        return false;
    }

    ClearAttributes(dst);

    RedirectEntry e;
    e.base = outName;
    e.to = dst;
    g_extraRedirects.push_back(e);
    g_extracted.push_back(dst);

    RememberCopy(dst, bytes);

    qeclog::Write(L"[file] 已从 DLL 释放 %s（%u 字节，已隐藏）", outName, static_cast<unsigned>(size));
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring ToLower(const std::wstring& s)
{
    std::wstring r = s;
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = static_cast<wchar_t>(towlower(r[i]));
    return r;
}

static bool FileExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string ReadAllBytes(const std::wstring& p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::string();

    std::string data;
    char buf[4096];
    DWORD read = 0;

    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0)
        data.append(buf, read);

    CloseHandle(h);
    return data;
}

static bool WriteAllBytes(const std::wstring& p, const std::string& data)
{
    // Clear hidden/read-only attributes first; both can make the write fail
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            FlushFileBuffers(h);
            CloseHandle(h);

            if (ok != FALSE)
                return true;
        }

        Sleep(250); // previous process may still hold the handle; wait and retry
    }

    return false;
}

static std::string Narrow(std::string s) { return s; }

static std::string NarrowW(const std::wstring& w)
{
    if (w.empty())
        return std::string();

    const int need = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0)
        return std::string();

    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), &out[0], need, nullptr, nullptr);
    return out;
}

static std::string TrimA2(std::string s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string LowerA2(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 128) s[i] = static_cast<char>(tolower(c));
    }
    return s;
}

// Split/join lines byte-wise so multibyte text is preserved
static std::vector<std::string> SplitLines2(const std::string& text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\n')
        {
            if (!cur.empty() && cur[cur.size() - 1] == '\r') cur.erase(cur.size() - 1);
            lines.push_back(cur);
            cur.clear();
        }
        else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

static std::string JoinLines2(const std::vector<std::string>& lines)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) { out += lines[i]; out += "\r\n"; }
    return out;
}

// Basename of a path, lowercased (case- and directory-insensitive comparison)
static std::string BaseNameLowerA(const std::string& in)
{
    std::string s = TrimA2(in);
    const size_t p = s.find_last_of("\\/");
    if (p != std::string::npos)
        s = s.substr(p + 1);
    return LowerA2(s);
}

// Rebuild the [Include] section: remove dropped files, move our entry last (later
// includes win) and renumber the rest 0..n-1 (the editor walks keys sequentially and
// a gap stops later entries from loading). Returns the number of dropped entries.
static int RewriteIncludeSection(std::string& text, const std::string& entry,
                                 const std::vector<std::string>& drops)
{
    std::vector<std::string> lines = SplitLines2(text);

    int secIdx = -1;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (LowerA2(TrimA2(lines[i])) == "[include]") { secIdx = static_cast<int>(i); break; }
    }

    // No [Include] section: create one holding only our entry
    if (secIdx < 0)
    {
        lines.push_back("[Include]");
        lines.push_back("0=" + entry);
        text = JoinLines2(lines);
        return 0;
    }

    // Section body spans [secIdx+1, endIdx)
    size_t endIdx = lines.size();
    for (size_t i = static_cast<size_t>(secIdx) + 1; i < lines.size(); ++i)
    {
        if (!TrimA2(lines[i]).empty() && TrimA2(lines[i])[0] == '[') { endIdx = i; break; }
    }

    const std::string entryLow = BaseNameLowerA(entry);
    std::vector<std::string> keep;     // kept entry values, renumbered
    std::vector<std::string> rawKeep;  // kept non key=value lines (comments), copied as-is
    int dropped = 0;

    for (size_t i = static_cast<size_t>(secIdx) + 1; i < endIdx; ++i)
    {
        const std::string t = TrimA2(lines[i]);
        if (t.empty())
            continue;

        const size_t eq = t.find('=');
        if (eq == std::string::npos)
        {
            rawKeep.push_back(lines[i]);
            continue;
        }

        const std::string val = TrimA2(t.substr(eq + 1));
        const std::string base = BaseNameLowerA(val);

        if (base == entryLow)      // our entry: appended last below
            continue;

        bool drop = false;
        for (size_t d = 0; d < drops.size(); ++d)
        {
            if (base == drops[d]) { drop = true; break; }
        }

        if (drop) { ++dropped; continue; }   // blocked file: removed from the list

        keep.push_back(val);
    }

    std::vector<std::string> rebuilt;
    for (size_t i = 0; i <= static_cast<size_t>(secIdx); ++i)
        rebuilt.push_back(lines[i]);

    char buf[32];
    for (size_t i = 0; i < keep.size(); ++i)
    {
        sprintf_s(buf, "%u=", static_cast<unsigned>(i));
        rebuilt.push_back(std::string(buf) + keep[i]);
    }

    sprintf_s(buf, "%u=", static_cast<unsigned>(keep.size()));
    rebuilt.push_back(std::string(buf) + entry);   // our entry always goes last

    for (size_t i = 0; i < rawKeep.size(); ++i)
        rebuilt.push_back(rawKeep[i]);

    for (size_t i = endIdx; i < lines.size(); ++i)
        rebuilt.push_back(lines[i]);

    text = JoinLines2(rebuilt);
    return dropped;
}

// Create empty (0-byte) shadow copies for blocked files
// Any other path that opens them still reads empty content, so no data reaches the editor
static void EnsureBlockedFiles()
{
    for (const std::wstring& name : g_drops)
    {
        const std::wstring dst = g_shadowDir + L"\\" + name;

        if (!WriteAllBytes(dst, std::string()))
        {
            qeclog::Write(L"[file] !! 生成空副本失败：%s", dst.c_str());
            continue;
        }

        ClearAttributes(dst);

        RedirectEntry e;
        e.base = name;
        e.to = dst;
        e.blocked = true;
        g_extraRedirects.push_back(e);

        qeclog::Write(L"[file] 空副本已就绪：%s（%s 的内容不会再传进地编）", dst.c_str(), name.c_str());
    }
}

// Build copies with the fixed include order and register their redirects
static void EnsureIncludeFix()
{
    g_extraRedirects.clear();

    std::vector<std::string> drops;
    for (const std::wstring& d : g_drops)
        drops.push_back(BaseNameLowerA(NarrowW(d)));   // lowercase, same form used for comparisons

    for (const FixEntry& fix : g_fixes)
    {
        // The original ini may have been moved into DataMain\, so check both locations
        const std::wstring src = IniSourcePath(fix.file);
        if (src.empty())
        {
            qeclog::Write(L"[file] 跳过 %s（地编目录和 DataMain\\ 里都没有）", fix.file.c_str());
            continue;
        }

        std::string text = ReadAllBytes(src);

        // Settings from earlier runs first, then the include list, so our include order
        // always wins over anything the preference store happens to hold
        OverlayStore(text);

        const std::string entry = NarrowW(fix.entry);
        const int dropped = RewriteIncludeSection(text, entry, drops);

        const std::wstring dst = g_shadowDir + L"\\" + fix.file;
        if (!WriteAllBytes(dst, text))
        {
            qeclog::Write(L"[file] !! 生成 %s 的副本失败", fix.file);
            continue;
        }

        ClearAttributes(dst);

        RedirectEntry e;
        e.base = fix.file;
        e.to = dst;
        g_extraRedirects.push_back(e);

        RememberCopy(dst, text);

        qeclog::Write(L"[file] %s：include 已重建（拦掉 %d 条，%s 排在最后）✓（副本 %s）",
                      fix.file.c_str(), dropped, fix.entry.c_str(), dst.c_str());
    }

    EnsureBlockedFiles();
}

static void EnsureDirectory(const std::wstring& dir)
{
    if (!dir.empty() && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(dir.c_str(), nullptr);
}

// Working copies have to stay writable: security software refuses every write to a file
// that carries the hidden attribute. Clear whatever attribute is set on the path.
static void ClearAttributes(const std::wstring& path)
{
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
}

// Whether the name ends with the given suffix, case-insensitively
static bool HasSuffixNoCase(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return (s.size() >= n) && (_wcsicmp(s.c_str() + (s.size() - n), suffix) == 0);
}

// Locate the original ini: prefer DataMain\ (filled at startup), then the editor directory
// GetFileAttributes bypasses our hook, so both locations must be checked here
static std::wstring IniSourcePath(const std::wstring& name)
{
    if (!g_dataDir.empty())
    {
        const std::wstring p = g_dataDir + L"\\" + name;
        if (FileExists(p))
            return p;
    }

    const std::wstring p = g_editorDir + L"\\" + name;
    return FileExists(p) ? p : std::wstring();
}

// ---------------------------------------------------------------------------
// On every start, move all *.ini from the editor directory into the data dir
// ([Paths] DataDir=). Only *.ini; log/txt/cfg stay put (aqrit.cfg must stay in the root).
// Must run before any of these inis are read.
// ---------------------------------------------------------------------------
static void MoveInisToIniDir()
{
    if (g_dataDir.empty())
    {
        qeclog::Write(L"[ini] [Paths] DataDir 留空 → ini 不搬家、不重定向");
        return;
    }

    EnsureDirectory(g_dataDir);

    WIN32_FIND_DATAW fd = { 0 };
    const std::wstring spec = g_editorDir + L"\\*.ini";
    HANDLE h = FindFirstFileW(spec.c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        qeclog::Write(L"[ini] 地编目录里没有 ini（都待在 %s 里 ✓）", g_dataDir.c_str());
        return;
    }

    int moved = 0;
    int failed = 0;

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;

        const std::wstring name = fd.cFileName;
        const std::wstring src = g_editorDir + L"\\" + name;
        const std::wstring dst = g_dataDir + L"\\" + name;

        // Whitelist: the preference store and the editor's own config stay in the editor
        // directory, so their option settings are stored and read back normally
        if (!g_userIni.empty() && _wcsicmp(name.c_str(), g_userIni.c_str()) == 0)
        {
            qeclog::Write(L"[ini] 保留 %s 在地编目录（偏好设置 / 最近文件存放处）✓", name.c_str());
            continue;
        }

        if (!g_lockedIni.empty() && _wcsicmp(name.c_str(), g_lockedIni.c_str()) == 0)
        {
            qeclog::Write(L"[ini] 保留 %s 在地编目录（地编自己的配置）✓", name.c_str());
            continue;
        }

        SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);

        // Overwrite an existing target: the root copy is the newest one
        if (MoveFileExW(src.c_str(), dst.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE)
        {
            ++moved;
        }
        else
        {
            ++failed;
            qeclog::Write(L"[ini] !! 搬不动 %s（错误码 %lu；编辑器还在跑？）",
                          name.c_str(), GetLastError());
        }
    }
    while (FindNextFileW(h, &fd) != FALSE);

    FindClose(h);

    if (moved > 0)
        qeclog::Write(L"[ini] 已把 %d 个 ini 搬进 %s\\ ✓", moved, g_dataDir.c_str());

    if (failed > 0)
        qeclog::Write(L"[ini] !! 有 %d 个 ini 没搬成", failed);
}

// ---------------------------------------------------------------------------
// Legacy directory name: merge <editor dir>\INIMain\ (old name) into the data dir.
// Only runs when the legacy directory exists; it is removed once emptied.
// ---------------------------------------------------------------------------
static void MigrateLegacyDataDir()
{
    const std::wstring legacyName = cfg::LegacyDataDir();

    if (legacyName.empty() || g_dataDir.empty())
        return;

    if (_wcsicmp(legacyName.c_str(), cfg::DataDir().c_str()) == 0)
        return;   // same name, nothing to merge

    const std::wstring legacy = g_editorDir + L"\\" + legacyName;

    if (GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    EnsureDirectory(g_dataDir);

    WIN32_FIND_DATAW fd = { 0 };
    HANDLE h = FindFirstFileW((legacy + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    int moved = 0;

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;

        const std::wstring src = legacy + L"\\" + fd.cFileName;
        const std::wstring dst = g_dataDir + L"\\" + fd.cFileName;

        SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);

        if (MoveFileExW(src.c_str(), dst.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE)
        {
            ++moved;
        }
    }
    while (FindNextFileW(h, &fd) != FALSE);

    FindClose(h);

    if (moved > 0)
        qeclog::Write(L"[data] 旧目录 %s\\ 里的 %d 个文件已并到 %s\\ ✓",
                      legacyName.c_str(), moved, g_dataDir.c_str());

    RemoveDirectoryW(legacy.c_str());   // removes only an empty directory
}

// ---------------------------------------------------------------------------
// Move listed files from the editor directory into a target folder (shared by
// DataMain\ and Others\). Only listed files are moved, and later opens are
// redirected there too (rules 4 and 5 in ShouldRedirect).
// ---------------------------------------------------------------------------
static void MoveListedToDir(const std::vector<std::wstring>& files,
                            const std::wstring& dstDir, const wchar_t* tag)
{
    if (files.empty() || dstDir.empty())
        return;

    EnsureDirectory(dstDir);

    for (const std::wstring& name : files)
    {
        if (name.empty())
            continue;

        if (name.find(L'\\') != std::wstring::npos ||
            name.find(L'/')  != std::wstring::npos ||
            name.find(L':')  != std::wstring::npos)
        {
            qeclog::Write(L"[%s] 跳过 %s（名单里只能写文件名）", tag, name.c_str());
            continue;
        }

        const std::wstring src = g_editorDir + L"\\" + name;
        const std::wstring dst = dstDir + L"\\" + name;

        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;   // already moved

        SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);

        if (MoveFileExW(src.c_str(), dst.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE)
            qeclog::Write(L"[%s] 已搬进 %s\\ ：%s", tag, dstDir.c_str(), name.c_str());
        else
            qeclog::Write(L"[%s] 搬不动 %s（错误码 %lu —— 被占用就留着，下次启动再试 ✓）",
                          tag, name.c_str(), GetLastError());
    }
}

// ---------------------------------------------------------------------------
// Detect dlls Syringe injects (they carry a .syhks00 section); these must never be
// moved, because Syringe only scans *.dll in the root directory.
// ---------------------------------------------------------------------------
static bool HasSyringeSection(const std::wstring& path)
{
    const std::string img = ReadAllBytes(path);
    if (img.size() < 0x100)
        return false;

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const size_t ntOff = static_cast<size_t>(dos->e_lfanew);
    if (ntOff + sizeof(IMAGE_NT_HEADERS32) > img.size())
        return false;

    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(img.data() + ntOff);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const size_t secOff = reinterpret_cast<const BYTE*>(sec) -
                          reinterpret_cast<const BYTE*>(img.data());

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (secOff + (i + 1) * sizeof(IMAGE_SECTION_HEADER) > img.size())
            break;

        if (memcmp(sec[i].Name, ".syhks00", 8) == 0)
            return true;
    }

    return false;
}

// Dlls imported by the host exe (the editor) must not be moved either; the loader
// needs them before Syringe runs (for example ddraw.dll)
static void CollectHostImports(std::vector<std::string>& out)
{
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    if (base == nullptr)
        return;

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return;

    const IMAGE_IMPORT_DESCRIPTOR* imp =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

    for (; imp->Name != 0; ++imp)
    {
        const char* name = reinterpret_cast<const char*>(base + imp->Name);
        out.push_back(LowerA2(std::string(name)));
    }
}

// ---------------------------------------------------------------------------
// Move movable dlls into DllMain\ at startup ([Search] DllDir=). Our DLL loads before
// FA2sp.dll, so the support libraries are not locked yet; SetDllDirectoryW above lets
// Syringe find them. Ourselves, .syhks00 dlls, host imports and locked files stay put.
// ---------------------------------------------------------------------------
static void MoveDllsToDllDir()
{
    if (g_dllDir.empty())
    {
        qeclog::Write(L"[dll] [Search] DllDir 留空 → dll 不搬家");
        return;
    }

    // Manual keep list ([Move] Keep=)
    std::vector<std::string> keepList;
    for (const std::wstring& k : cfg::All(L"Move", L"Keep"))
    {
        if (!k.empty())
            keepList.push_back(LowerA2(NarrowW(k)));
    }

    // Automatic list: host imports
    std::vector<std::string> hostImports;
    CollectHostImports(hostImports);

    const std::string selfLow = LowerA2(NarrowW(g_selfName));

    EnsureDirectory(g_dllDir);

    WIN32_FIND_DATAW fd = { 0 };
    const std::wstring spec = g_editorDir + L"\\*.dll";
    HANDLE h = FindFirstFileW(spec.c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        qeclog::Write(L"[dll] 地编目录里没有 dll（都待在 %s 里 ✓）", g_dllDir.c_str());
        return;
    }

    int moved = 0;

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;

        const std::wstring name = fd.cFileName;
        const std::wstring src = g_editorDir + L"\\" + name;
        const std::string low = LowerA2(NarrowW(name));

        // 1) ourselves
        if (!selfLow.empty() && low == selfLow)
        {
            qeclog::Write(L"[dll] 保留 %s（就是我们自己 ✓）", name.c_str());
            continue;
        }

        // 2) keep list
        bool keep = false;
        for (const std::string& k : keepList)
        {
            if (k == low) { keep = true; break; }
        }
        if (keep)
        {
            qeclog::Write(L"[dll] 保留 %s（白名单 [Move] Keep=）", name.c_str());
            continue;
        }

        // 3) imported by the host exe; the loader needs it before Syringe
        keep = false;
        for (const std::string& i : hostImports)
        {
            if (i == low) { keep = true; break; }
        }
        if (keep)
        {
            qeclog::Write(L"[dll] 保留 %s（地编本体导入 —— 加载器比 Syringe 更早要它 ✗）", name.c_str());
            continue;
        }

        // 4) injected by Syringe (.syhks00 section)
        if (HasSyringeSection(src))
        {
            qeclog::Write(L"[dll] 保留 %s（带 .syhks00 段 —— Syringe 只在根目录扫 ✗）", name.c_str());
            continue;
        }

        // everything else can be moved
        const std::wstring dst = g_dllDir + L"\\" + name;
        SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);

        if (MoveFileExW(src.c_str(), dst.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE)
        {
            ++moved;
            qeclog::Write(L"[dll] 已搬进 %s\\ ：%s", g_dllDir.c_str(), name.c_str());
        }
        else
        {
            qeclog::Write(L"[dll] 搬不动 %s（错误码 %lu —— 正在使用中就留着 ✓）",
                          name.c_str(), GetLastError());
        }
    }
    while (FindNextFileW(h, &fd) != FALSE);

    FindClose(h);

    if (moved == 0)
        qeclog::Write(L"[dll] 没有需要搬的 dll ✓");
}

// ---------------------------------------------------------------------------
// 1.5 s after startup, log the path each key dll was loaded from. DllMain runs too
// early (FA2sp is not loaded yet), so this is deferred to a thread. The log exposes
// any future load-order breakage that would hide the DllMain\ support libraries.
// ---------------------------------------------------------------------------
static DWORD WINAPI ModuleCheckProc(LPVOID /*param*/)
{
    Sleep(1500);

    static const wchar_t* kNames[] =
    {
        L"FA2sp.dll", L"lua.dll", L"libcrypto-1_1.dll", L"Lexilla.dll",
        L"CncVxlRenderText.dll", L"Scintilla.dll", L"ddraw.dll"
    };

    for (const wchar_t* n : kNames)
    {
        HMODULE h = GetModuleHandleW(n);
        if (h == nullptr)
        {
            qeclog::Write(L"[dll]   %-22s **没加载**", n);
            continue;
        }

        wchar_t path[MAX_PATH] = { 0 };
        GetModuleFileNameW(h, path, MAX_PATH);
        qeclog::Write(L"[dll]   %-22s ← %s", n, path);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Add the subfolder to the Windows DLL search path ([Search] DllDir=). Subfolders are
// not searched by default, and FA2sp.dll's static dependencies live there. Our
// placeholder hook is the lowest, so this runs before Syringe loads FA2sp.dll
// (see src\syringe_hook.h / syringe_section.cpp).
// ---------------------------------------------------------------------------
static void SetupDllSearchDir()
{
    const std::wstring name = cfg::S(L"Search", L"DllDir", L"DllMain");

    if (name.empty())
    {
        qeclog::Write(L"[dll] [Search] DllDir 留空 → 不加 DLL 搜索目录");
        return;
    }

    if (name.find(L'\\') != std::wstring::npos ||
        name.find(L'/')  != std::wstring::npos ||
        name.find(L':')  != std::wstring::npos)
    {
        qeclog::Write(L"[dll] !! [Search] DllDir 只能写文件夹名（不带路径）：%s", name.c_str());
        return;
    }

    const std::wstring dir = g_editorDir + L"\\" + name;
    EnsureDirectory(dir);

    g_dllDir = dir;   // also used for dll moves

    if (SetDllDirectoryW(dir.c_str()) != FALSE)
        qeclog::Write(L"[dll] DLL 搜索目录已加入：%s ✓（支持库 dll 住在里面）", dir.c_str());
    else
        qeclog::Write(L"[dll] !! SetDllDirectoryW 失败：%s（错误码 %lu）", dir.c_str(), GetLastError());
}

// ---------------------------------------------------------------------------
// Handle editor-directory files listed in [Delete]. Entries are moved to Others\ like
// [Others]; nothing is deleted, so files stay recoverable. Scans only the editor
// directory itself and supports * and ?; wildcard entries skip executable/resource
// extensions, and moved files are registered for redirection into Others\.
// ---------------------------------------------------------------------------
static bool IsProtectedExtension(const std::wstring& name)
{
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;

    const std::wstring ext = ToLower(name.substr(dot));

    static const wchar_t* kBad[] =
    {
        L".exe", L".dll", L".dat", L".mix", L".log",
        L".pdb", L".sys", L".ocx", L".cpl", L".com", L".bat", L".cmd"
    };

    for (const wchar_t* b : kBad)
    {
        if (ext == b)
            return true;
    }
    return false;
}

static void MoveListedPatternsToOthers()
{
    const std::vector<std::wstring> files = cfg::All(L"Delete", L"File");

    if (files.empty() || g_othersDir.empty())
        return;

    EnsureDirectory(g_othersDir);

    for (const std::wstring& pat : files)
    {
        if (pat.empty())
            continue;

        // File names only: entries containing a path are skipped to prevent directory traversal
        if (pat.find(L'\\') != std::wstring::npos ||
            pat.find(L'/')  != std::wstring::npos ||
            pat.find(L':')  != std::wstring::npos)
        {
            qeclog::Write(L"[others] 跳过 \"%s\"：只能写文件名，不能带路径", pat.c_str());
            continue;
        }

        const bool wildcard = (pat.find(L'*') != std::wstring::npos) ||
                              (pat.find(L'?') != std::wstring::npos);

        WIN32_FIND_DATAW fd = { 0 };
        const std::wstring spec = g_editorDir + L"\\" + pat;
        HANDLE h = FindFirstFileW(spec.c_str(), &fd);

        if (h == INVALID_HANDLE_VALUE)
        {
            qeclog::Write(L"[others] \"%s\"：地编目录里没有匹配的文件", pat.c_str());
            continue;
        }

        int moved = 0;

        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;

            const std::wstring name = fd.cFileName;
            const std::wstring src = g_editorDir + L"\\" + name;

            // Extension protection applies to wildcard entries only; an exact name is explicit
            if (wildcard && IsProtectedExtension(name))
            {
                qeclog::Write(L"[others] 跳过 %s（通配符不搬可执行/资源类文件 ✗；要搬请写准确文件名）",
                              name.c_str());
                continue;
            }

            const std::wstring dst = g_othersDir + L"\\" + name;

            SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);

            if (MoveFileExW(src.c_str(), dst.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE)
            {
                ++moved;

                // Register for redirection so later opens of this name go to the Others folder
                const std::string low = LowerA2(NarrowW(name));
                bool has = false;
                for (const std::string& f : g_otherFileLows)
                {
                    if (f == low) { has = true; break; }
                }
                if (!has)
                    g_otherFileLows.push_back(low);

                qeclog::Write(L"[others] 已搬进 %s\\ ：%s", g_othersDir.c_str(), name.c_str());
            }
            else
            {
                qeclog::Write(L"[others] 搬不动 %s（错误码 %lu —— 被占用就留着，下次启动再试 ✓）",
                              name.c_str(), GetLastError());
            }
        }
        while (FindNextFileW(h, &fd) != FALSE);

        FindClose(h);

        if (moved == 0)
            qeclog::Write(L"[others] \"%s\"：没有需要搬的文件", pat.c_str());
    }
}

// ---------------------------------------------------------------------------
// Shadow config: editor state plus forced overrides from the built-in config
// ---------------------------------------------------------------------------
static bool RefreshShadow()
{
    // Directory for shadow files
    wchar_t localAppData[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0)
    {
        if (GetTempPathW(MAX_PATH, localAppData) == 0)
            return false;
    }

    std::wstring dir = localAppData;
    if (!dir.empty() && dir[dir.size() - 1] == L'\\')
        dir.erase(dir.size() - 1);

    dir += L"\\";
    dir += g_shadowDirName;
    EnsureDirectory(dir);

    g_shadowDir = dir;
    g_shadowPath = dir + L"\\" + g_shadowName;

    // Start from the existing shadow file to keep editor state (recent files, dialogs)
    std::string base;
    if (FileExists(g_shadowPath))
        base = ReadAllBytes(g_shadowPath);

    // The visible FinalAlert.ini holds the option settings and nothing else; it is the
    // newest state, because Uninstall writes it back on every exit.
    if (!g_lockedIni.empty() && !g_editorDir.empty())
    {
        const std::wstring visible = g_editorDir + L"\\" + g_lockedIni;
        if (FileExists(visible))
        {
            const std::string opts = ReadAllBytes(visible);
            if (!opts.empty())
            {
                base = qecini::MergeTextIntoIniText(base, opts);
                qeclog::Write(L"[file] 影子配置已并入 %s（%u 字节的选项设置）",
                              g_lockedIni.c_str(), static_cast<unsigned>(opts.size()));
            }
        }
    }

    // Overlay the preference store: preferences may be written through the profile API
    // (which keeps them in that ini) while the same ini is read through the editor's own
    // reader (which sees this shadow), so both views have to agree.
    if (!g_userIniPath.empty() && FileExists(g_userIniPath))
    {
        const std::string prefs = ReadAllBytes(g_userIniPath);
        if (!prefs.empty())
        {
            base = qecini::MergeTextIntoIniText(base, prefs);
            qeclog::Write(L"[file] 影子配置已并入 %s（%u 字节）",
                          g_userIni.c_str(), static_cast<unsigned>(prefs.size()));
        }
    }

    // Force the built-in config in
    const std::string merged = qecini::MergeIntoIniText(base);

    if (!WriteAllBytes(g_shadowPath, merged))
    {
        qeclog::Write(L"[file] !! 写影子配置失败：%s", g_shadowPath.c_str());
        return false;
    }

    // Mark hidden so Explorer does not show it by default
    ClearAttributes(g_shadowPath);

    qeclog::Write(L"[file] 影子配置已就绪：%s（%u 字节，已隐藏）",
                  g_shadowPath.c_str(), static_cast<unsigned>(merged.size()));
    return true;
}

// Path of the shadow config, so the ini interception can read values that were written
// through the editor's own file reader (which is redirected here)
std::wstring ShadowConfigPath()
{
    return g_shadowPath;
}

// ---------------------------------------------------------------------------
// Decide whether a path must be redirected
// ---------------------------------------------------------------------------
// kind: 0 = shadow config / derived copy     1 = blanked     2 = data dir     3 = others
static bool ShouldRedirect(const std::wstring& name, std::wstring& out, int& kind)
{
    kind = 0;

    if (name.empty() || g_shadowPath.empty())
        return false;

    // Resolve the full path; relative paths resolve against the working directory
    wchar_t full[MAX_PATH * 2] = { 0 };
    if (GetFullPathNameW(name.c_str(), MAX_PATH * 2, full, nullptr) == 0)
        return false;

    const std::wstring fullPath(full);

    // Already a shadow path: never redirect again or it would loop
    if (ToLower(fullPath) == ToLower(g_shadowPath))
        return false;

    // Compare by file name
    const size_t slash = fullPath.find_last_of(L"\\/");
    const std::wstring base = (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);
    const std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : fullPath.substr(0, slash);

    // Only the editor directory itself; files in subfolders are left alone
    const bool inEditorDir = !g_editorDir.empty() && (_wcsicmp(dir.c_str(), g_editorDir.c_str()) == 0);

    // 1) include-order fixes and blanked files (FAData.ini / FALanguage.ini / qec01.dat ...)
    if (inEditorDir)
    {
        for (const RedirectEntry& e : g_extraRedirects)
        {
            if (_wcsicmp(base.c_str(), e.base.c_str()) == 0 && _wcsicmp(fullPath.c_str(), e.to.c_str()) != 0)
            {
                out = e.to;
                kind = e.blocked ? 1 : 0;
                return true;
            }
        }
    }

    // 2) the editor's own config is served from the shadow copy: it holds the built-in
    //    paths/language plus the option settings, so no path ever lands in the visible
    //    FinalAlert.ini (see Uninstall, which writes the options back without them)
    if (inEditorDir && _wcsicmp(base.c_str(), g_lockedIni.c_str()) == 0)
    {
        out = g_shadowPath;
        kind = 0;
        return true;
    }

    // 2b) the preference store is opened where it is: in the editor directory
    if (inEditorDir && !g_userIni.empty() && _wcsicmp(base.c_str(), g_userIni.c_str()) == 0)
        return false;

    // 3) other *.ini files go to the data dir
    //    All ini reads and writes go through CreateFile, so they all land there
    //    (checked after 1 and 2, which have their own destinations)
    if (inEditorDir && !g_dataDir.empty() && HasSuffixNoCase(base, L".ini"))
    {
        out = g_dataDir + L"\\" + base;
        kind = 2;
        return true;
    }

    // 4) [DataFiles] entries (currently only Quantum.rpck) go to the data dir
    // 5) [Others] entries (logs and similar) go to the Others folder
    //    Bare file names still present in old inis resolve this way; nothing else is touched
    if (inEditorDir)
    {
        const std::string lowBase = LowerA2(NarrowW(base));
        bool hit = false;

        if (!g_dataDir.empty())
        {
            for (const std::string& f : g_dataFileLows)
            {
                if (f == lowBase)
                {
                    out = g_dataDir + L"\\" + base;
                    kind = 2;
                    hit = true;
                    break;
                }
            }
        }

        if (!hit && !g_othersDir.empty())
        {
            for (const std::string& f : g_otherFileLows)
            {
                if (f == lowBase)
                {
                    out = g_othersDir + L"\\" + base;
                    kind = 3;
                    hit = true;
                    break;
                }
            }
        }

        if (hit)
            return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

// Diagnostic: with an empty FA2Entropy.trace file next to the DLL, every file the editor
// opens for writing is written to the log, so it is easy to see which ini holds a setting.
// It changes nothing about the editor's behaviour.
static bool TraceWrites()
{
    static int cached = -1;
    if (cached < 0)
    {
        const std::wstring marker = g_editorDir + L"\\" + ENTROPY_TRACE_MARKER;
        cached = (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    }
    return cached == 1;
}

static void TraceWrite(const wchar_t* what, const wchar_t* path, DWORD access, DWORD disp)
{
    if (!TraceWrites())
        return;

    const bool write = ((access & (GENERIC_WRITE | FILE_WRITE_DATA)) != 0);
    if (!write)
        return;

    qeclog::Write(L"[trace] %s 写入打开：%s（access=0x%08X disp=%u）", what, path, access, disp);
}

// Same, with the outcome and the resolved path (the editor may pass a bare name that the
// system resolves somewhere else, e.g. into the Windows directory)
static void TraceResult(const wchar_t* what, const wchar_t* path, DWORD access, DWORD disp, HANDLE result)
{
    if (!TraceWrites())
        return;

    const bool write = ((access & (GENERIC_WRITE | FILE_WRITE_DATA)) != 0);
    if (!write)
        return;

    wchar_t full[MAX_PATH * 2] = { 0 };
    const DWORD n = GetFullPathNameW(path, MAX_PATH * 2, full, nullptr);
    const wchar_t* resolved = (n > 0 && n < MAX_PATH * 2) ? full : path;

    if (result != INVALID_HANDLE_VALUE)
    {
        qeclog::Write(L"[trace] %s 写入打开成功：%s（access=0x%08X disp=%u）",
                      what, resolved, access, disp);
    }
    else
    {
        qeclog::Write(L"[trace] %s 写入打开失败（错误 %lu）：%s（access=0x%08X disp=%u）",
                      what, GetLastError(), resolved, access, disp);
    }
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    if (name != nullptr)
    {
        std::wstring to;
        int kind = 0;
        if (ShouldRedirect(name, to, kind))
        {
            if (kind == 1)
                qeclog::Write(L"[file] 重定向（置空）：%s → %s", name, to.c_str());
            else if (kind == 2)
                qeclog::Write(L"[file] 重定向（DataMain）：%s → %s", name, to.c_str());
            else if (kind == 3)
                qeclog::Write(L"[file] 重定向（Others）：%s → %s", name, to.c_str());
            else
                qeclog::Write(L"[file] 重定向（影子配置）：%s → %s", name, to.c_str());

            TraceWrite(L"(已重定向)", to.c_str(), access, disp);
            const HANDLE h = g_CreateFileW(to.c_str(), access, share, sa, disp, flags, tmpl);
            TraceResult(L"(已重定向)", to.c_str(), access, disp, h);
            return h;
        }

        TraceWrite(L"", name, access, disp);
        const HANDLE h = g_CreateFileW(name, access, share, sa, disp, flags, tmpl);
        TraceResult(L"", name, access, disp, h);
        return h;
    }

    return g_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    if (name != nullptr)
    {
        std::wstring wname;
        const int need = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
        if (need > 1)
        {
            wname.resize(static_cast<size_t>(need - 1));
            MultiByteToWideChar(CP_ACP, 0, name, -1, &wname[0], need);
        }

        std::wstring to;
        int kind = 0;
        if (!wname.empty() && ShouldRedirect(wname, to, kind))
        {
            if (kind == 1)
                qeclog::Write(L"[file] 重定向（置空）：%S → %s", name, to.c_str());
            else if (kind == 2)
                qeclog::Write(L"[file] 重定向（DataMain）：%S → %s", name, to.c_str());
            else if (kind == 3)
                qeclog::Write(L"[file] 重定向（Others）：%S → %s", name, to.c_str());
            else
                qeclog::Write(L"[file] 重定向（影子配置）：%S → %s", name, to.c_str());

            return g_CreateFileW(to.c_str(), access, share, sa, disp, flags, tmpl);
        }

        if (!wname.empty())
        {
            TraceWrite(L"", wname.c_str(), access, disp);
            const HANDLE h = g_CreateFileW(wname.c_str(), access, share, sa, disp, flags, tmpl);
            TraceResult(L"", wname.c_str(), access, disp, h);
            return h;
        }
    }

    return g_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------
bool Install(HMODULE self)
{
    // ---- read the lists from assets\FA2Entropy.ini ----
    g_shadowDirName = cfg::ShadowDir();
    g_shadowName    = cfg::ShadowFile();
    g_lockedIni     = cfg::LockedIni();
    g_userIni       = cfg::UserIni();

    // [DataFiles] / [Others] lists (lowercase names, for moving and redirection)
    g_dataFileLows.clear();
    for (const std::wstring& f : cfg::DataFileList())
    {
        if (!f.empty())
            g_dataFileLows.push_back(LowerA2(NarrowW(f)));
    }

    g_otherFileLows.clear();
    for (const std::wstring& f : cfg::OtherFileList())
    {
        if (!f.empty())
            g_otherFileLows.push_back(LowerA2(NarrowW(f)));
    }

    g_fixes.clear();
    for (const cfg::FixItem& f : cfg::FixList())
    {
        FixEntry e;
        e.file  = f.file;
        e.entry = f.entry;
        g_fixes.push_back(e);
    }

    g_drops = cfg::DropList();

    // Remember the editor directory
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring path(exePath);
    const size_t slash = path.find_last_of(L"\\/");
    g_editorDir = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);

    // Our own file name, skipped when moving dlls
    {
        wchar_t selfPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(self, selfPath, MAX_PATH);
        const std::wstring sp(selfPath);
        const size_t s = sp.find_last_of(L"\\/");
        g_selfName = (s == std::wstring::npos) ? sp : sp.substr(s + 1);
    }

    // Preference store path; a bare file name sits next to the editor (see [Lock] UserIni=)
    if (!g_userIni.empty())
    {
        const bool bare = (g_userIni.find(L'\\') == std::wstring::npos) &&
                          (g_userIni.find(L'/')  == std::wstring::npos) &&
                          (g_userIni.find(L':')  == std::wstring::npos);

        g_userIniPath = bare ? (g_editorDir + L"\\" + g_userIni) : g_userIni;
    }

    // Step 1: add the subfolder to the DLL search path
    // Must happen before Syringe calls LoadLibrary(FA2sp.dll); we load first, so it does
    SetupDllSearchDir();

    // Step 2: move movable dlls into DllMain\ while they are still unloaded
    MoveDllsToDllDir();

    // Step 3: move all inis into the data dir before anything reads them
    const std::wstring iniDirName = cfg::DataDir();

    if (!iniDirName.empty() &&
        iniDirName.find(L'\\') == std::wstring::npos &&
        iniDirName.find(L'/')  == std::wstring::npos &&
        iniDirName.find(L':')  == std::wstring::npos)
    {
        g_dataDir = g_editorDir + L"\\" + iniDirName;
    }
    else if (!iniDirName.empty())
    {
        qeclog::Write(L"[ini] !! [Paths] DataDir 只能写文件夹名（不带路径）：%s", iniDirName.c_str());
    }

    // [Others] target directory for extra files
    {
        const std::wstring othersName = cfg::OthersDir();

        if (!othersName.empty() &&
            othersName.find(L'\\') == std::wstring::npos &&
            othersName.find(L'/')  == std::wstring::npos &&
            othersName.find(L':')  == std::wstring::npos)
        {
            g_othersDir = g_editorDir + L"\\" + othersName;
        }
    }

    // Step 4: one-time migration of the legacy directory name
    MigrateLegacyDataDir();

    // Step 5: move explicitly listed files first
    //   [DataFiles] -> data dir, [Others] and [Delete] -> the Others folder
    //   Runs before the bulk ini move so named files such as FinalAlert.ini keep their destination
    MoveListedToDir(cfg::DataFileList(),  g_dataDir,   L"data");
    MoveListedToDir(cfg::OtherFileList(), g_othersDir, L"others");
    MoveListedPatternsToOthers();

    // Step 6: move the remaining *.ini files into the data dir before they are read
    MoveInisToIniDir();

    if (!RefreshShadow())
        return false;

    EnsureIncludeFix();

    // The adapted inis come from DLL resources; the release package does not need them.
    // Output names come from [Extract]; the editor opens them by the [Include] Fix= name.
    for (const cfg::ExtractItem& it : cfg::ExtractList())
        ExtractAsset(self, it.resId, it.name.c_str());

    // ([Delete] files were already moved to Others\ in step 5)

    // MinHook may already be initialized by ini_lock; ALREADY_INITIALIZED is fine here
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        qeclog::Write(L"[file] !! MH_Initialize 失败：%d", static_cast<int>(initStatus));
        return false;
    }

    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr)
        return false;

    LPVOID tW = reinterpret_cast<LPVOID>(GetProcAddress(k32, "CreateFileW"));
    LPVOID tA = reinterpret_cast<LPVOID>(GetProcAddress(k32, "CreateFileA"));

    bool ok = true;

    if (tW != nullptr)
    {
        const MH_STATUS s = MH_CreateHook(tW, reinterpret_cast<LPVOID>(&Hook_CreateFileW),
                                          reinterpret_cast<LPVOID*>(&g_CreateFileW));
        ok = ok && (s == MH_OK || s == MH_ERROR_ALREADY_CREATED);
    }

    if (tA != nullptr)
    {
        const MH_STATUS s = MH_CreateHook(tA, reinterpret_cast<LPVOID>(&Hook_CreateFileA),
                                          reinterpret_cast<LPVOID*>(&g_CreateFileA));
        ok = ok && (s == MH_OK || s == MH_ERROR_ALREADY_CREATED);
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        ok = false;

    qeclog::Write(L"[file] 文件重定向 = %s", ok ? L"已启用 ✓" : L"有失败 ✗");

    // Run the module check 1.5 s later, once FA2sp has loaded
    CloseHandle(CreateThread(nullptr, 0, ModuleCheckProc, nullptr, 0, nullptr));

    return ok;
}

// On uninstall, strip the built-in keys from the shadow file so no paths remain on disk.
// They are required while the editor runs and are re-injected on the next start.
void Uninstall()
{
    if (g_shadowPath.empty() || !FileExists(g_shadowPath))
        return;

    // Save what the editor changed in the working copies before they are deleted below.
    // Without this, preferences it wrote into them would be lost on every run.
    std::string diffAll;

    for (const ManagedCopy& c : g_managed)
    {
        if (!FileExists(c.path))
            continue;

        const std::string now = ReadAllBytes(c.path);
        if (now.empty())
            continue;

        const std::string diff = qecini::DiffIniText(c.baseline, now);
        if (diff.empty())
            continue;

        diffAll += diff;

        qeclog::Write(L"[ini] %s 里有改动，已记进偏好设置 ✓", c.path.c_str());
    }

    if (!diffAll.empty() && !g_userIniPath.empty())
    {
        std::string store;
        if (FileExists(g_userIniPath))
            store = ReadAllBytes(g_userIniPath);

        const std::string merged = qecini::MergeTextIntoIniText(store, diffAll);

        if (WriteAllBytes(g_userIniPath, merged))
            qeclog::Write(L"[ini] 偏好设置已保存到 %s ✓", g_userIni.c_str());
        else
            qeclog::Write(L"[ini] !! 保存 %s 失败", g_userIni.c_str());
    }

    // Delete all derived copies; they are regenerated on the next start
    // (extracted Quantum01/02.ini and the include-fixed FAData/FALanguage copies)
    for (const RedirectEntry& e : g_extraRedirects)
    {
        SetFileAttributesW(e.to.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(e.to.c_str());
    }
    g_extraRedirects.clear();
    g_extracted.clear();
    g_managed.clear();

    const std::string base = ReadAllBytes(g_shadowPath);
    const std::string stripped = qecini::StripFromIniText(base);

    if (WriteAllBytes(g_shadowPath, stripped))
    {
        ClearAttributes(g_shadowPath);
        qeclog::Write(L"[file] 已抹掉影子文件里的路径/语言（磁盘上不再留明文）✓");
    }
    else
    {
        qeclog::Write(L"[file] !! 抹除影子文件失败（留着也不影响功能）");
    }

    // And into the visible config the editor reads: option settings only. Anything the
    // built-in config defines (the resource path, the language) is stripped from it first
    // and never written, so the released editor keeps those hidden.
    //
    // This is the only file the option settings are written to, so the editor directory
    // holds exactly one ini. The preference store only comes into play for data files
    // whose changes are captured above.
    if (!g_lockedIni.empty() && !g_editorDir.empty())
    {
        const std::wstring visible = g_editorDir + L"\\" + g_lockedIni;

        std::string old;
        if (FileExists(visible))
            old = qecini::StripFromIniText(ReadAllBytes(visible));

        const std::string merged = qecini::FormatIniText(
            stripped.empty() ? old : qecini::MergeTextIntoIniText(old, stripped));

        if (WriteAllBytes(visible, merged))
        {
            // Never leave a path in the file the user can open
            const std::string clean = qecini::FormatIniText(
                qecini::StripFromIniText(ReadAllBytes(visible)));

            if (clean != merged)
                WriteAllBytes(visible, clean);

            qeclog::Write(L"[ini] %s 已更新（%u 字节，只含选项设置，不含路径）✓",
                          g_lockedIni.c_str(), static_cast<unsigned>(clean.size()));
        }
        else
        {
            qeclog::Write(L"[ini] !! 写 %s 失败", g_lockedIni.c_str());
        }
    }
}

} // namespace qecfile
