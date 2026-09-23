// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  FinalAlert.ini read/write interception
//
//  Config source: the INI hardcoded in the DLL (see embedded_ini.h); no dependency
//  on any ini on disk.
//
//  Policy:
//    Read: requested section/key exists in the embedded config -> return the embedded
//          value without touching the file. Whole-section or section-name enumeration
//          -> merge the embedded keys and sections in. Other keys (recent files,
//          tip positions, editor state) -> pass through unchanged.
//    Write: target is FinalAlert.ini -> swallow it (report success, write nothing),
//           so our keys can never reach the file.
//
//  Hooks only the kernel32 profile API; no exe-internal addresses, no FA2sp version checks.
// =============================================================================

#include "ini_lock.h"
#include "lock_config.h"
#include "entropy_cfg.h"
#include "file_redirect.h"
#include "log.h"

#include "MinHook.h"

#include <windows.h>
#include <string>
#include <vector>

namespace qecini {

// ---------------------------------------------------------------------------
// Original function pointers
// ---------------------------------------------------------------------------
typedef UINT  (WINAPI *PFN_GetStrW)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, UINT, LPCWSTR);
typedef UINT  (WINAPI *PFN_GetStrA)(LPCSTR,  LPCSTR,  LPCSTR,  LPSTR,  UINT, LPCSTR);
typedef UINT  (WINAPI *PFN_GetIntW)(LPCWSTR, LPCWSTR, INT, LPCWSTR);
typedef UINT  (WINAPI *PFN_GetIntA)(LPCSTR,  LPCSTR,  INT, LPCSTR);
typedef DWORD (WINAPI *PFN_GetSecW)(LPCWSTR, LPWSTR, DWORD, LPCWSTR);
typedef DWORD (WINAPI *PFN_GetSecA)(LPCSTR,  LPSTR,  DWORD, LPCSTR);
typedef DWORD (WINAPI *PFN_GetNamesW)(LPWSTR, DWORD, LPCWSTR);
typedef DWORD (WINAPI *PFN_GetNamesA)(LPSTR,  DWORD, LPCSTR);
typedef BOOL  (WINAPI *PFN_WriteStrW)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
typedef BOOL  (WINAPI *PFN_WriteStrA)(LPCSTR,  LPCSTR,  LPCSTR,  LPCSTR);
typedef BOOL  (WINAPI *PFN_WriteSecW)(LPCWSTR, LPCWSTR, LPCWSTR);
typedef BOOL  (WINAPI *PFN_WriteSecA)(LPCSTR,  LPCSTR,  LPCSTR);

static PFN_GetStrW    g_GetStrW    = nullptr;
static PFN_GetStrA    g_GetStrA    = nullptr;
static PFN_GetIntW    g_GetIntW    = nullptr;
static PFN_GetIntA    g_GetIntA    = nullptr;
static PFN_GetSecW    g_GetSecW    = nullptr;
static PFN_GetSecA    g_GetSecA    = nullptr;
static PFN_GetNamesW  g_GetNamesW  = nullptr;
static PFN_GetNamesA  g_GetNamesA  = nullptr;
static PFN_WriteStrW  g_WriteStrW  = nullptr;
static PFN_WriteStrA  g_WriteStrA  = nullptr;
static PFN_WriteSecW  g_WriteSecW  = nullptr;
static PFN_WriteSecA  g_WriteSecA  = nullptr;

// ---------------------------------------------------------------------------
// Embedded config (parsed from QEC_EMBEDDED_INI)
// ---------------------------------------------------------------------------
struct IniEntry
{
    std::wstring section;
    std::wstring key;
    std::wstring value;
};

static std::vector<IniEntry> g_blob;
static std::vector<std::wstring> g_blobSections;
static bool g_blobReady = false;

static void Trim(std::wstring& s)
{
    const wchar_t* ws = L" \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos)
    {
        s.clear();
        return;
    }
    const size_t e = s.find_last_not_of(ws);
    s = s.substr(b, e - b + 1);
}

// Parse "section / key=value" text. Only key=value lines are kept; section names are
// collected in order of appearance. Used for the built-in editor config and for the
// preference store read from disk.
static void ParseIniEntries(const std::wstring& text,
                            std::vector<IniEntry>& entries,
                            std::vector<std::wstring>* sections)
{
    std::wstring section;

    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t eol = text.find(L'\n', pos);
        std::wstring line = (eol == std::wstring::npos)
            ? text.substr(pos)
            : text.substr(pos, eol - pos);

        pos = (eol == std::wstring::npos) ? (text.size() + 1) : (eol + 1);

        Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#')
            continue;

        if (line[0] == L'[')
        {
            const size_t close = line.find(L']');
            if (close != std::wstring::npos)
            {
                section = line.substr(1, close - 1);
                Trim(section);

                if (sections != nullptr)
                {
                    bool exists = false;
                    for (const std::wstring& s : *sections)
                    {
                        if (_wcsicmp(s.c_str(), section.c_str()) == 0)
                        {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists)
                        sections->push_back(section);
                }
            }
            continue;
        }

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;

        IniEntry e;
        e.section = section;
        e.key = line.substr(0, eq);
        e.value = line.substr(eq + 1);
        Trim(e.key);
        Trim(e.value);

        if (!e.key.empty())
            entries.push_back(e);
    }
}

static void EnsureBlob()
{
    if (g_blobReady)
        return;

    g_blobReady = true;

    // Config comes from the DLL resource (assets\FinalAlert.ini); only key=value lines are parsed, nothing is read from disk.
    ParseIniEntries(cfg::WideFromBytes(cfg::EditorIniBytes()), g_blob, &g_blobSections);

    qeclog::Write(L"[ini] 内置配置已加载：%u 个键，%u 个段",
                  static_cast<unsigned>(g_blob.size()),
                  static_cast<unsigned>(g_blobSections.size()));
}

static bool SameName(const wchar_t* a, const wchar_t* b)
{
    return (a != nullptr) && (b != nullptr) && (_wcsicmp(a, b) == 0);
}

static const std::wstring* FindBlobValue(const wchar_t* app, const wchar_t* key)
{
    if (app == nullptr || key == nullptr)
        return nullptr;

    EnsureBlob();

    for (const IniEntry& e : g_blob)
    {
        if (SameName(app, e.section.c_str()) && SameName(key, e.key.c_str()))
            return &e.value;
    }

    return nullptr;
}

// Whether this section exists in the embedded config
static bool BlobHasSection(const wchar_t* app)
{
    if (app == nullptr)
        return false;

    EnsureBlob();

    for (const std::wstring& s : g_blobSections)
    {
        if (SameName(app, s.c_str()))
            return true;
    }
    return false;
}

// Locked ini name / shadow file name, read once from config at startup
static std::wstring g_lockedIni;
static std::wstring g_shadowName;

// Preference store ([Lock] UserIni=): the non-fixed keys of the locked ini are kept
// in this file, in the editor directory, so editor preferences and recent files keep
// working while the fixed keys stay fixed.
static std::wstring g_userIni;       // file name
static std::wstring g_userIniPath;   // full path next to the editor

// Shadow config ([Shadow]): the editor's own file reader/writer is redirected there, so
// values written that way must be readable here as well.
static std::wstring g_shadowPath;

// Ini directory ([Paths] IniDir=) and editor directory, needed for profile API path rewriting
static std::wstring g_dataDir;
static std::wstring g_editorDir;

// ---------------------------------------------------------------------------
// Redirect an ini path into DataMain\ when needed.
//
//   Needed because some FA2sp code (e.g. Translations.cpp) reads and writes the ini
//   through GetPrivateProfile* / WritePrivateProfile*, which bypass the CreateFile
//   hook, so the path argument must be replaced here too.
//
//   Returns the rewritten path, or file unchanged when no rewrite applies.
// ---------------------------------------------------------------------------
static const wchar_t* RewriteIniPath(const wchar_t* file, std::wstring& storage)
{
    if (file == nullptr || g_dataDir.empty() || g_editorDir.empty())
        return file;

    wchar_t full[MAX_PATH * 2] = { 0 };
    if (GetFullPathNameW(file, MAX_PATH * 2, full, nullptr) == 0)
        return file;

    const std::wstring path(full);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return file;

    const std::wstring dir  = path.substr(0, slash);
    const std::wstring base = path.substr(slash + 1);

    // Only paths directly in the editor directory are rewritten; paths already under DataMain\ are left alone.
    if (_wcsicmp(dir.c_str(), g_editorDir.c_str()) != 0)
        return file;

    const size_t dot = base.find_last_of(L'.');
    if (dot == std::wstring::npos || _wcsicmp(base.c_str() + dot, L".ini") != 0)
        return file;

    // The locked ini has its own logic (serve embedded values / block writes), so leave it alone here.
    if (_wcsicmp(base.c_str(), g_lockedIni.c_str()) == 0)
        return file;

    // The preference store stays in the editor directory on purpose: never rewrite it.
    if (!g_userIni.empty() && _wcsicmp(base.c_str(), g_userIni.c_str()) == 0)
        return file;

    storage = g_dataDir + L"\\" + base;
    return storage.c_str();
}

// File name of a path, without the directory (both slash kinds count)
static const wchar_t* BaseNameOf(const wchar_t* path)
{
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p != 0; ++p)
    {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }
    return base;
}

// Compares file names only, ignoring the directory; case-insensitive, both slash kinds count
static bool IsLockedFile(const wchar_t* path)
{
    if (path == nullptr)
        return false;

    const wchar_t* base = BaseNameOf(path);

    // Two names match: the editor's own ini (config [Lock] Ini=) and our shadow file
    return (_wcsicmp(base, g_lockedIni.c_str()) == 0) ||
           (_wcsicmp(base, g_shadowName.c_str()) == 0);
}

// Whether the path names the preference store ([Lock] UserIni=)
static bool IsUserIniFile(const wchar_t* path)
{
    if (path == nullptr || g_userIni.empty())
        return false;

    return _wcsicmp(BaseNameOf(path), g_userIni.c_str()) == 0;
}

// Copies at most the caller's buffer size, always NUL-terminates, returns the characters written (excluding NUL)
static UINT CopyOut(const wchar_t* src, size_t len, LPWSTR out, UINT size)
{
    if (out == nullptr || size == 0)
        return 0;

    UINT n = static_cast<UINT>(len);
    if (n > size - 1)
        n = size - 1;

    if (n > 0)
        memcpy(out, src, static_cast<size_t>(n) * sizeof(wchar_t));

    out[n] = 0;
    return n;
}

static std::wstring A2W(const char* s)
{
    if (s == nullptr)
        return std::wstring();

    const int need = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (need <= 1)
        return std::wstring();

    std::wstring out(static_cast<size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], need);
    return out;
}

static std::string W2A(const wchar_t* s, size_t len)
{
    if (s == nullptr || len == 0)
        return std::string();

    const int need = WideCharToMultiByte(CP_ACP, 0, s, static_cast<int>(len),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0)
        return std::string();

    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_ACP, 0, s, static_cast<int>(len), &out[0], need, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Merge the embedded keys of this section in (replace existing, append missing).
// Section format: "key=value\0key=value\0"; this function appends the final NUL.
// ---------------------------------------------------------------------------
static void MergeSection(const wchar_t* app, std::wstring& section)
{
    typedef std::pair<std::wstring, std::wstring> Item;
    std::vector<Item> items;

    size_t i = 0;
    while (i < section.size())
    {
        const size_t end = section.find(L'\0', i);
        if (end == std::wstring::npos || end == i)
            break;

        std::wstring line = section.substr(i, end - i);
        const size_t eq = line.find(L'=');
        if (eq != std::wstring::npos)
            items.push_back(Item(line.substr(0, eq), line.substr(eq + 1)));

        i = end + 1;
    }

    bool anyBlob = false;

    if (app != nullptr)
    {
        EnsureBlob();

        for (const IniEntry& e : g_blob)
        {
            if (!SameName(app, e.section.c_str()))
                continue;

            bool replaced = false;
            for (Item& it : items)
            {
                if (SameName(it.first.c_str(), e.key.c_str()))
                {
                    it.second = e.value;
                    replaced = true;
                }
            }

            if (!replaced)
                items.push_back(Item(e.key, e.value));

            anyBlob = true;
        }

        if (anyBlob)
            qeclog::Write(L"[ini] 读取段 [%s] → 已把内置配置的键并进去", app);
    }

    std::wstring out;
    for (const Item& it : items)
    {
        out += it.first;
        out += L'=';
        out += it.second;
        out += L'\0';
    }
    out += L'\0';

    section.swap(out);
}

// ---------------------------------------------------------------------------
// Fixed keys and the preference store
// ---------------------------------------------------------------------------

// A key we fix: it is defined by the built-in editor config, so the editor must not
// change it. Editing a fixed section as a whole (key == nullptr) counts as fixed too.
static bool IsFixedKey(const wchar_t* app, const wchar_t* key)
{
    if (app == nullptr)
        return false;

    if (key == nullptr)
        return BlobHasSection(app);

    return FindBlobValue(app, key) != nullptr;
}

// Read a key that we do not fix. The shadow config is what the editor reads and writes,
// so it answers first; the visible config next to the editor and the preference store are
// fallbacks for settings that were stored there.
static UINT ReadNonFixedStr(const wchar_t* app, const wchar_t* key, LPCWSTR def,
                            LPWSTR out, UINT size, const wchar_t* original)
{
    const wchar_t* sources[3] = { g_shadowPath.c_str(), original, g_userIniPath.c_str() };

    for (int i = 0; i < 3; ++i)
    {
        if (sources[i][0] == 0)
            continue;

        // An empty default tells us whether the key is there at all
        const UINT n = g_GetStrW(app, key, L"", out, size, sources[i]);
        if (n > 0)
            return n;
    }

    return g_GetStrW(app, key, (def != nullptr) ? def : L"", out, size, original);
}

// Same for a whole section: the first source that has the section wins. The built-in keys
// are merged in on top, so fixed values always win.
//
// Enumeration goes through GetPrivateProfileSection: GetPrivateProfileString with a NULL
// key does not enumerate a section on current Windows, it just returns an empty result.
static UINT ReadNonFixedSection(const wchar_t* app, LPWSTR out, UINT size, const wchar_t* original)
{
    const wchar_t* sources[3] = { g_shadowPath.c_str(), original, g_userIniPath.c_str() };

    std::vector<wchar_t> tmp(32768, 0);
    DWORD n = 0;

    for (int i = 0; i < 3 && n == 0; ++i)
    {
        if (sources[i][0] == 0)
            continue;

        n = g_GetSecW(app, tmp.data(), static_cast<DWORD>(tmp.size()), sources[i]);
    }

    std::wstring sec(tmp.data(), n);
    sec.push_back(L'\0');
    MergeSection(app, sec);

    return CopyOut(sec.c_str(), sec.size() - 1, out, size);
}

// ---------------------------------------------------------------------------
// Read: string
// ---------------------------------------------------------------------------
static UINT WINAPI Hook_GetStrW(LPCWSTR app, LPCWSTR key, LPCWSTR def,
                                LPWSTR out, UINT size, LPCWSTR file)
{
    std::wstring rewritten;
    const wchar_t* f = RewriteIniPath(file, rewritten);

    if (IsLockedFile(f))
    {
        if (key == nullptr)
        {
            // Whole-section request: take the section, then merge the built-in keys in
            return ReadNonFixedSection(app, out, size, f);
        }

        if (const std::wstring* locked = FindBlobValue(app, key))
        {
            qeclog::Write(L"[ini] 读取 [%s] %s → 返回内置值（不读文件）",
                          app ? app : L"?", key);
            return CopyOut(locked->c_str(), locked->size(), out, size);
        }

        // Not a fixed key: preferences and recent files come from the store or the shadow
        return ReadNonFixedStr(app, key, def, out, size, f);
    }

    return g_GetStrW(app, key, def, out, size, f);
}

static UINT WINAPI Hook_GetStrA(LPCSTR app, LPCSTR key, LPCSTR def,
                                LPSTR out, UINT size, LPCSTR file)
{
    const std::wstring wfile = A2W(file);

    if (IsLockedFile(wfile.c_str()))
    {
        const std::wstring wapp = A2W(app);
        const std::wstring wkey = A2W(key);
        const std::wstring wdef = A2W(def);

        std::vector<wchar_t> wout(size > 0 ? size : 1, 0);
        const UINT n = Hook_GetStrW(app ? wapp.c_str() : nullptr,
                                    key ? wkey.c_str() : nullptr,
                                    def ? wdef.c_str() : nullptr,
                                    wout.data(), size, wfile.c_str());

        if (out == nullptr || size == 0)
            return 0;

        const std::string a = W2A(wout.data(), n);
        const UINT len = static_cast<UINT>(a.size());
        const UINT copy = (len > size - 1) ? size - 1 : len;

        if (copy > 0)
            memcpy(out, a.data(), copy);
        out[copy] = 0;
        return copy;
    }

    return g_GetStrA(app, key, def, out, size, file);
}

// ---------------------------------------------------------------------------
// Read: integer (wraps the string version)
// ---------------------------------------------------------------------------
static UINT WINAPI Hook_GetIntW(LPCWSTR app, LPCWSTR key, INT def, LPCWSTR file)
{
    std::wstring rewritten;
    const wchar_t* f = RewriteIniPath(file, rewritten);

    if (IsLockedFile(f))
    {
        if (FindBlobValue(app, key) != nullptr)
        {
            wchar_t buf[64] = { 0 };
            Hook_GetStrW(app, key, L"", buf, 64, f);
            return static_cast<UINT>(wcstol(buf, nullptr, 10));
        }

        // Not a fixed key: the shadow config answers (it is what the editor reads)
        if (!g_shadowPath.empty())
        {
            wchar_t buf[64] = { 0 };
            if (g_GetStrW(app, key, L"", buf, 64, g_shadowPath.c_str()) > 0)
                return static_cast<UINT>(wcstol(buf, nullptr, 10));
        }

        if (!g_userIniPath.empty())
        {
            wchar_t buf[64] = { 0 };
            if (g_GetStrW(app, key, L"", buf, 64, g_userIniPath.c_str()) > 0)
                return static_cast<UINT>(wcstol(buf, nullptr, 10));
        }

        return g_GetIntW(app, key, def, f);
    }

    return g_GetIntW(app, key, def, f);
}

static UINT WINAPI Hook_GetIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file)
{
    const std::wstring wfile = A2W(file);

    // The string hook holds the whole policy, so route every locked-ini read through it
    if (IsLockedFile(wfile.c_str()))
    {
        const std::wstring wapp = A2W(app);
        const std::wstring wkey = A2W(key);

        return Hook_GetIntW(app ? wapp.c_str() : nullptr,
                            key ? wkey.c_str() : nullptr,
                            def, wfile.c_str());
    }

    return g_GetIntA(app, key, def, file);
}

// ---------------------------------------------------------------------------
// Read: whole section / section names
// ---------------------------------------------------------------------------
static DWORD WINAPI Hook_GetSecW(LPCWSTR app, LPWSTR out, DWORD size, LPCWSTR file)
{
    std::wstring rewritten;
    const wchar_t* f = RewriteIniPath(file, rewritten);

    // Fixed keys win; every other key of that section comes from the store or the shadow
    if (IsLockedFile(f) && app != nullptr)
        return ReadNonFixedSection(app, out, size, f);

    return g_GetSecW(app, out, size, f);
}

static DWORD WINAPI Hook_GetSecA(LPCSTR app, LPSTR out, DWORD size, LPCSTR file)
{
    const std::wstring wfile = A2W(file);

    if (IsLockedFile(wfile.c_str()) && app != nullptr)
    {
        const std::wstring wapp = A2W(app);
        std::vector<wchar_t> wout(size > 0 ? size : 1, 0);
        const DWORD n = Hook_GetSecW(wapp.c_str(), wout.data(), size, wfile.c_str());

        if (out == nullptr || size == 0)
            return 0;

        const std::string a = W2A(wout.data(), n);
        const DWORD copy = (a.size() > size - 1) ? size - 1 : static_cast<DWORD>(a.size());
        if (copy > 0)
            memcpy(out, a.data(), copy);
        out[copy] = 0;
        return copy;
    }

    return g_GetSecA(app, out, size, file);
}

// Append the section names of an ini, skipping names that are already in the list
static void AppendSectionNames(const wchar_t* path, std::vector<std::wstring>& names)
{
    std::vector<wchar_t> tmp(32768, 0);
    const DWORD n = g_GetNamesW(tmp.data(), static_cast<DWORD>(tmp.size()), path);

    const std::wstring all(tmp.data(), n);
    size_t i = 0;

    while (i < all.size())
    {
        const size_t end = all.find(L'\0', i);
        if (end == std::wstring::npos || end == i)
            break;

        const std::wstring name = all.substr(i, end - i);

        bool found = false;
        for (const std::wstring& have : names)
        {
            if (SameName(have.c_str(), name.c_str()))
            {
                found = true;
                break;
            }
        }

        if (!found)
            names.push_back(name);

        i = end + 1;
    }
}

static DWORD WINAPI Hook_GetNamesW(LPWSTR out, DWORD size, LPCWSTR file)
{
    std::wstring rewritten;
    const wchar_t* f = RewriteIniPath(file, rewritten);

    if (!IsLockedFile(f))
        return g_GetNamesW(out, size, f);

    // Sections of the named file, plus the ones the store and the shadow hold
    std::vector<std::wstring> names;
    AppendSectionNames(f, names);

    if (!g_userIniPath.empty())
        AppendSectionNames(g_userIniPath.c_str(), names);

    if (!g_shadowPath.empty())
        AppendSectionNames(g_shadowPath.c_str(), names);

    // Append embedded sections that neither of them lists yet
    EnsureBlob();
    for (const std::wstring& s : g_blobSections)
    {
        bool found = false;
        for (const std::wstring& have : names)
        {
            if (SameName(have.c_str(), s.c_str()))
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            names.push_back(s);
            qeclog::Write(L"[ini] 段名列表补上 [%s]（文件里没有）", s.c_str());
        }
    }

    std::wstring rebuilt;
    for (const std::wstring& s : names)
    {
        rebuilt += s;
        rebuilt += L'\0';
    }
    rebuilt += L'\0';

    return CopyOut(rebuilt.c_str(), rebuilt.size() - 1, out, size);
}

static DWORD WINAPI Hook_GetNamesA(LPSTR out, DWORD size, LPCSTR file)
{
    const std::wstring wfile = A2W(file);

    if (!IsLockedFile(wfile.c_str()))
        return g_GetNamesA(out, size, file);

    std::vector<wchar_t> wout(size > 0 ? size : 1, 0);
    const DWORD n = Hook_GetNamesW(wout.data(), size, wfile.c_str());

    if (out == nullptr || size == 0)
        return 0;

    const std::string a = W2A(wout.data(), n);
    const DWORD copy = (a.size() > size - 1) ? size - 1 : static_cast<DWORD>(a.size());
    if (copy > 0)
        memcpy(out, a.data(), copy);
    out[copy] = 0;
    return copy;
}

// ---------------------------------------------------------------------------
// Write
//
//   Fixed keys (language, resource paths) are dropped, so the editor cannot change them.
//   Everything else is written to the editor's own ini, exactly where the editor and
//   FA2sp expect their option settings, and mirrored into the preference store so a
//   setting captured by an older layout is not lost.
// ---------------------------------------------------------------------------
static BOOL WriteNonFixedStr(LPCWSTR app, LPCWSTR key, LPCWSTR val, const wchar_t* original)
{
    const BOOL ok = g_WriteStrW(app, key, val, original);

    if (!ok)
        qeclog::Write(L"[ini] !! 写入 %s 失败（错误 %lu）", original, GetLastError());

    if (!g_userIniPath.empty() && _wcsicmp(original, g_userIniPath.c_str()) != 0)
        g_WriteStrW(app, key, val, g_userIniPath.c_str());

    return ok;
}

static BOOL WriteNonFixedSection(LPCWSTR app, LPCWSTR sec, const wchar_t* original)
{
    const BOOL ok = g_WriteSecW(app, sec, original);

    if (!ok)
        qeclog::Write(L"[ini] !! 整段写入 %s 失败（错误 %lu）", original, GetLastError());

    if (!g_userIniPath.empty() && _wcsicmp(original, g_userIniPath.c_str()) != 0)
        g_WriteSecW(app, sec, g_userIniPath.c_str());

    return ok;
}

static BOOL WINAPI Hook_WriteStrW(LPCWSTR app, LPCWSTR key, LPCWSTR val, LPCWSTR file)
{
    std::wstring rewritten;
    const wchar_t* f = RewriteIniPath(file, rewritten);

    if (IsLockedFile(f))
    {
        if (IsFixedKey(app, key))
        {
            qeclog::Write(L"[ini] 拦截写入（固定键，不落盘）：[%s] %s",
                          app ? app : L"?", key ? key : L"(整段)");
            return TRUE; // Report success without writing
        }

        qeclog::Write(L"[ini] 写入改存 %s（并同步影子配置）：[%s] %s",
                      g_userIni.c_str(), app ? app : L"?", key ? key : L"(整段)");
        return WriteNonFixedStr(app, key, val, f);
    }

    return g_WriteStrW(app, key, val, f);
}

static BOOL WINAPI Hook_WriteStrA(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file)
{
    const std::wstring wfile = A2W(file);

    if (!IsLockedFile(wfile.c_str()))
        return g_WriteStrA(app, key, val, file);

    // The wide hook holds the whole policy; keep NULL as NULL so deletes stay deletes
    const std::wstring wapp = A2W(app);
    const std::wstring wkey = A2W(key);
    const std::wstring wval = A2W(val);

    return Hook_WriteStrW(app ? wapp.c_str() : nullptr,
                          key ? wkey.c_str() : nullptr,
                          val ? wval.c_str() : nullptr,
                          wfile.c_str());
}

static BOOL WINAPI Hook_WriteSecW(LPCWSTR app, LPCWSTR sec, LPCWSTR file)
{
    std::wstring rewritten;
    const wchar_t* f = RewriteIniPath(file, rewritten);

    if (IsLockedFile(f))
    {
        if (BlobHasSection(app))
        {
            qeclog::Write(L"[ini] 拦截整段写入（固定段，不落盘）：[%s]", app ? app : L"?");
            return TRUE;
        }

        qeclog::Write(L"[ini] 整段写入改存 %s（并同步影子配置）：[%s]",
                      g_userIni.c_str(), app ? app : L"?");
        return WriteNonFixedSection(app, sec, f);
    }

    return g_WriteSecW(app, sec, f);
}

static BOOL WINAPI Hook_WriteSecA(LPCSTR app, LPCSTR sec, LPCSTR file)
{
    const std::wstring wfile = A2W(file);

    if (!IsLockedFile(wfile.c_str()))
        return g_WriteSecA(app, sec, file);

    const std::wstring wapp = A2W(app);
    const std::wstring wsec = A2W(sec);

    return Hook_WriteSecW(app ? wapp.c_str() : nullptr,
                          sec ? wsec.c_str() : nullptr,
                          wfile.c_str());
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------
struct HookDef
{
    const char* name;
    LPVOID      detour;
    LPVOID*     original;
};

bool Install()
{
    g_lockedIni  = cfg::LockedIni();
    g_shadowName = cfg::ShadowFile();
    g_userIni    = cfg::UserIni();

    // Shadow config path: the file-level fallback for reads, built by qecfile::Install
    g_shadowPath = qecfile::ShadowConfigPath();

    // Ini directory ([Paths] IniDir=); the profile API rewrites paths into it
    {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        const std::wstring path(exePath);
        const size_t slash = path.find_last_of(L"\\/");
        g_editorDir = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);

        // Preference store: a bare file name sits next to the editor, a path is used as written
        if (!g_userIni.empty())
        {
            const bool bare = (g_userIni.find(L'\\') == std::wstring::npos) &&
                              (g_userIni.find(L'/')  == std::wstring::npos) &&
                              (g_userIni.find(L':')  == std::wstring::npos);

            g_userIniPath = bare ? (g_editorDir + L"\\" + g_userIni) : g_userIni;

            qeclog::Write(L"[ini] 偏好设置 / 最近文件等非固定键存放在：%s ✓", g_userIniPath.c_str());
        }

        const std::wstring iniDirName = cfg::DataDir();

        if (!iniDirName.empty() &&
            iniDirName.find(L'\\') == std::wstring::npos &&
            iniDirName.find(L'/')  == std::wstring::npos &&
            iniDirName.find(L':')  == std::wstring::npos)
        {
            g_dataDir = g_editorDir + L"\\" + iniDirName;
            qeclog::Write(L"[ini] profile API 的 ini 路径也会改写到：%s ✓", g_dataDir.c_str());
        }
    }

    EnsureBlob();

    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        qeclog::Write(L"[ini] !! MH_Initialize 失败：%d", static_cast<int>(st));
        return false;
    }

    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr)
    {
        qeclog::Write(L"[ini] !! 找不到 kernel32.dll");
        return false;
    }

    const HookDef defs[] =
    {
        { "GetPrivateProfileStringW",        reinterpret_cast<LPVOID>(&Hook_GetStrW),   reinterpret_cast<LPVOID*>(&g_GetStrW)   },
        { "GetPrivateProfileStringA",        reinterpret_cast<LPVOID>(&Hook_GetStrA),   reinterpret_cast<LPVOID*>(&g_GetStrA)   },
        { "GetPrivateProfileIntW",           reinterpret_cast<LPVOID>(&Hook_GetIntW),   reinterpret_cast<LPVOID*>(&g_GetIntW)   },
        { "GetPrivateProfileIntA",           reinterpret_cast<LPVOID>(&Hook_GetIntA),   reinterpret_cast<LPVOID*>(&g_GetIntA)   },
        { "GetPrivateProfileSectionW",       reinterpret_cast<LPVOID>(&Hook_GetSecW),   reinterpret_cast<LPVOID*>(&g_GetSecW)   },
        { "GetPrivateProfileSectionA",       reinterpret_cast<LPVOID>(&Hook_GetSecA),   reinterpret_cast<LPVOID*>(&g_GetSecA)   },
        { "GetPrivateProfileSectionNamesW",  reinterpret_cast<LPVOID>(&Hook_GetNamesW), reinterpret_cast<LPVOID*>(&g_GetNamesW) },
        { "GetPrivateProfileSectionNamesA",  reinterpret_cast<LPVOID>(&Hook_GetNamesA), reinterpret_cast<LPVOID*>(&g_GetNamesA) },
        { "WritePrivateProfileStringW",      reinterpret_cast<LPVOID>(&Hook_WriteStrW), reinterpret_cast<LPVOID*>(&g_WriteStrW) },
        { "WritePrivateProfileStringA",      reinterpret_cast<LPVOID>(&Hook_WriteStrA), reinterpret_cast<LPVOID*>(&g_WriteStrA) },
        { "WritePrivateProfileSectionW",     reinterpret_cast<LPVOID>(&Hook_WriteSecW), reinterpret_cast<LPVOID*>(&g_WriteSecW) },
        { "WritePrivateProfileSectionA",     reinterpret_cast<LPVOID>(&Hook_WriteSecA), reinterpret_cast<LPVOID*>(&g_WriteSecA) },
    };

    bool allOk = true;

    for (const HookDef& d : defs)
    {
        LPVOID target = reinterpret_cast<LPVOID>(GetProcAddress(k32, d.name));
        if (target == nullptr)
        {
            qeclog::Write(L"[ini] !! 找不到导出函数 %S", d.name);
            allOk = false;
            continue;
        }

        const MH_STATUS cs = MH_CreateHook(target, d.detour, d.original);
        if (cs != MH_OK && cs != MH_ERROR_ALREADY_CREATED)
        {
            qeclog::Write(L"[ini] !! 挂钩 %S 失败：%d", d.name, static_cast<int>(cs));
            allOk = false;
        }
    }

    const MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
    if (es != MH_OK)
    {
        qeclog::Write(L"[ini] !! MH_EnableHook 失败：%d", static_cast<int>(es));
        allOk = false;
    }

    qeclog::Write(L"[ini] hook 结果 = %s", allOk ? L"全部成功" : L"有失败（见上面）");
    return allOk;
}

void Uninstall()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

// ---------------------------------------------------------------------------
// Merge the embedded config into an ANSI ini text, used for the shadow config.
// The editor's own INI reader cannot be hooked, so it is handed a file with the correct contents.
// ---------------------------------------------------------------------------
static std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return std::string();

    const int need = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0)
        return std::string();

    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                        &out[0], need, nullptr, nullptr);
    return out;
}

static std::string LowerA(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
    return s;
}

static void TrimA(std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
    {
        s.clear();
        return;
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
}

// Merge the given entries into an ini text (ANSI), replacing existing keys and
// appending the missing ones.
static std::string MergeEntriesIntoIniText(const std::string& base, const std::vector<IniEntry>& entries)
{
    // Split into lines
    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < base.size(); ++i)
        {
            const char c = base[i];
            if (c == '\n')
            {
                if (!cur.empty() && cur[cur.size() - 1] == '\r')
                    cur.erase(cur.size() - 1);
                lines.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
    }

    // For each embedded key: replace it when the section already has it, otherwise append it
    for (const IniEntry& e : entries)
    {
        const std::string sec = Narrow(e.section);
        const std::string key = Narrow(e.key);
        const std::string val = Narrow(e.value);
        const std::string secLow = LowerA(sec);
        const std::string keyLow = LowerA(key);

        int secIdx = -1;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string t = lines[i];
            TrimA(t);
            if (t.size() >= 2 && t[0] == '[' && t[t.size() - 1] == ']')
            {
                if (LowerA(t.substr(1, t.size() - 2)) == secLow)
                {
                    secIdx = static_cast<int>(i);
                    break;
                }
            }
        }

        if (secIdx < 0)
        {
            lines.push_back("[" + sec + "]");
            lines.push_back(key + "=" + val);
            continue;
        }

        int keyIdx = -1;
        for (size_t i = static_cast<size_t>(secIdx) + 1; i < lines.size(); ++i)
        {
            std::string t = lines[i];
            TrimA(t);
            if (!t.empty() && t[0] == '[')
                break;

            const std::string lt = LowerA(t);
            const std::string prefix = keyLow + "=";
            if (lt.compare(0, prefix.size(), prefix) == 0)
            {
                keyIdx = static_cast<int>(i);
                break;
            }
        }

        if (keyIdx >= 0)
            lines[keyIdx] = key + "=" + val;
        else
            lines.insert(lines.begin() + secIdx + 1, key + "=" + val);
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        out += lines[i];
        out += "\r\n";
    }
    return out;
}

std::string MergeIntoIniText(const std::string& base)
{
    EnsureBlob();
    return MergeEntriesIntoIniText(base, g_blob);
}

std::string MergeTextIntoIniText(const std::string& base, const std::string& extra)
{
    // The preference store may be ANSI (WritePrivateProfile*A) or UTF-16 (the W variant);
    // cfg::WideFromBytes detects both.
    std::vector<IniEntry> entries;
    ParseIniEntries(cfg::WideFromBytes(extra), entries, nullptr);

    if (entries.empty())
        return base;

    return MergeEntriesIntoIniText(base, entries);
}

// One blank line between sections, no repeated or trailing blank lines
std::string FormatIniText(const std::string& text)
{
    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '\n')
            {
                if (!cur.empty() && cur[cur.size() - 1] == '\r')
                    cur.erase(cur.size() - 1);
                lines.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
    }

    std::vector<std::string> out;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string t = lines[i];
        TrimA(t);

        const bool isHeader = (t.size() >= 2 && t[0] == '[' && t[t.size() - 1] == ']');

        if (isHeader)
        {
            if (!out.empty())
            {
                std::string prev = out.back();
                TrimA(prev);
                if (!prev.empty())
                    out.push_back("");   // one blank line before a section
            }
        }
        else if (t.empty())
        {
            if (out.empty())
                continue;                // no leading blank lines

            std::string prev = out.back();
            TrimA(prev);
            if (prev.empty())
                continue;                // no repeated blank lines
        }

        out.push_back(lines[i]);
    }

    while (!out.empty())
    {
        std::string t = out.back();
        TrimA(t);
        if (t.empty())
            out.pop_back();
        else
            break;
    }

    std::string res;
    for (const std::string& l : out)
    {
        res += l;
        res += "\r\n";
    }
    return res;
}

std::string DiffIniText(const std::string& baseline, const std::string& current)
{
    std::vector<IniEntry> base;
    std::vector<IniEntry> cur;
    std::vector<std::wstring> sections;

    ParseIniEntries(cfg::WideFromBytes(baseline), base, nullptr);
    ParseIniEntries(cfg::WideFromBytes(current), cur, &sections);

    std::string out;
    std::string openSection;

    for (const std::wstring& s : sections)
    {
        bool wroteHeader = false;

        for (const IniEntry& c : cur)
        {
            if (_wcsicmp(c.section.c_str(), s.c_str()) != 0)
                continue;

            // Unchanged when the baseline holds the same key with the same value
            bool changed = true;
            int  seen = 0;

            for (const IniEntry& b : base)
            {
                if (_wcsicmp(b.section.c_str(), c.section.c_str()) == 0 &&
                    _wcsicmp(b.key.c_str(), c.key.c_str()) == 0)
                {
                    ++seen;
                    if (seen == 1)
                        changed = (_wcsicmp(b.value.c_str(), c.value.c_str()) != 0);
                }
            }

            // A key that the baseline repeats (FA2 data files use e.g. "+=TYPE" lists) is
            // not a value that can be carried over, so leave those alone
            if (seen > 1)
                continue;

            if (!changed)
                continue;

            if (!wroteHeader)
            {
                out += "[" + Narrow(s) + "]\r\n";
                wroteHeader = true;
            }

            out += Narrow(c.key) + "=" + Narrow(c.value) + "\r\n";
        }
    }

    return out;
}

std::string StripFromIniText(const std::string& text)
{
    EnsureBlob();

    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '\n')
            {
                if (!cur.empty() && cur[cur.size() - 1] == '\r')
                    cur.erase(cur.size() - 1);
                lines.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
    }

    std::string section;
    std::vector<std::string> keep;

    for (const std::string& l : lines)
    {
        std::string t = l;
        TrimA(t);

        if (t.size() >= 2 && t[0] == '[' && t[t.size() - 1] == ']')
        {
            section = LowerA(t.substr(1, t.size() - 2));
            keep.push_back(l);
            continue;
        }

        bool drop = false;
        const size_t eq = t.find('=');
        if (eq != std::string::npos)
        {
            const std::string keyLow = LowerA(t.substr(0, eq));

            for (const IniEntry& e : g_blob)
            {
                if (LowerA(Narrow(e.section)) == section && LowerA(Narrow(e.key)) == keyLow)
                {
                    drop = true;
                    break;
                }
            }
        }

        if (!drop)
            keep.push_back(l);
    }

    // Drop section headers that lost every key, so the file the user can open does not
    // advertise sections whose contents are kept hidden
    std::vector<std::string> cleaned;

    for (size_t i = 0; i < keep.size(); ++i)
    {
        std::string t = keep[i];
        TrimA(t);

        const bool isHeader = (t.size() >= 2 && t[0] == '[' && t[t.size() - 1] == ']');
        if (!isHeader)
        {
            cleaned.push_back(keep[i]);
            continue;
        }

        bool hasKey = false;
        for (size_t j = i + 1; j < keep.size(); ++j)
        {
            std::string tj = keep[j];
            TrimA(tj);

            if (tj.empty() || tj[0] == ';' || tj[0] == '#')
                continue;

            if (tj[0] == '[')
                break;   // next section

            if (tj.find('=') != std::string::npos)
            {
                hasKey = true;
                break;
            }
        }

        if (hasKey)
            cleaned.push_back(keep[i]);
    }

    std::string out;
    for (const std::string& l : cleaned)
    {
        out += l;
        out += "\r\n";
    }
    return out;
}
} // namespace qecini
