// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

#include "entropy_cfg.h"
#include "lock_config.h"
#include "log.h"

#include <windows.h>
#include <stdlib.h>

#include <string>
#include <vector>

namespace cfg {

// ---------------------------------------------------------------------------
// Defaults, used only when the matching entry is missing from the DLL resources.
// Change configuration in the assets\ ini files, not here.
// ---------------------------------------------------------------------------
static const wchar_t* DEF_HOST_EXE       = L"FA2Entropy.dat";
static const unsigned DEF_HOST_STAMP     = 0x3EDB6BD4u;
static const wchar_t* DEF_SHADOW_DIR     = L"FA2Entropy";
static const wchar_t* DEF_SHADOW_FILE    = L"fa2entropy.dat";
static const wchar_t* DEF_LOCKED_INI     = L"FinalAlert.ini";
static const wchar_t* DEF_USER_INI       = L"FA2Entropy.ini";
static const wchar_t* DEF_INI_DIR        = L"DataMain";
static const wchar_t* DEF_LEGACY_DIR     = L"INIMain";

static const wchar_t* DEF_DATA_FILES[] =
{
    L"Quantum.rpck",
};

static const wchar_t* DEF_OTHERS_DIR   = L"Others";

static const wchar_t* DEF_FIX[][2] =
{
    { L"FAData.ini",     L"qec01.dat" },
    { L"FALanguage.ini", L"qec02.dat" },
};

static const wchar_t* DEF_DROP[] =
{
    L"FAData_TriggerAndScript.ini",
    L"Quantum01.ini",
    L"Quantum02.ini",
};

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
struct Entry
{
    std::wstring section;
    std::wstring key;
    std::wstring value;
};

static std::string        g_editorIni;      // raw bytes of assets\FinalAlert.ini
static std::vector<Entry> g_items;          // parsed assets\FA2Entropy.ini
static bool               g_loaded = false;
static bool               g_haveDirectives = false;

// ---------------------------------------------------------------------------
// Resource loading
// ---------------------------------------------------------------------------
bool LoadResourceBytes(HMODULE self, int id, std::string& out)
{
    const HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (res == nullptr)
        return false;

    const HGLOBAL h = LoadResource(self, res);
    if (h == nullptr)
        return false;

    const void* data = LockResource(h);
    const DWORD size = SizeofResource(self, res);

    if (data == nullptr || size == 0)
        return false;

    out.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(size));
    return true;
}

std::wstring WideFromBytes(const std::string& bytes)
{
    size_t off = 0;

    // UTF-16 with BOM (what the Unicode profile API writes): decode it as it is.
    // Without this, such a file would be read as ANSI and turn into garbage.
    if (bytes.size() >= 2)
    {
        const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
        const unsigned char b1 = static_cast<unsigned char>(bytes[1]);

        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF))
        {
            const bool bigEndian = (b0 == 0xFE);

            std::wstring out;
            out.reserve((bytes.size() - 2) / 2);

            for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                const unsigned lo = static_cast<unsigned char>(bytes[i]);
                const unsigned hi = static_cast<unsigned char>(bytes[i + 1]);
                out.push_back(static_cast<wchar_t>(bigEndian ? ((lo << 8) | hi) : ((hi << 8) | lo)));
            }
            return out;
        }
    }

    // Skip the UTF-8 BOM (added by Notepad).
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        off = 3;
    }

    if (bytes.size() <= off)
        return std::wstring();

    const char* p = bytes.data() + off;
    const int   len = static_cast<int>(bytes.size() - off);

    // Try UTF-8 first; fall back to ANSI/GBK when the bytes are not valid UTF-8.
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int need = MultiByteToWideChar(CP_UTF8, flags, p, len, nullptr, 0);

    if (need <= 0)
    {
        cp = CP_ACP;
        flags = 0;
        need = MultiByteToWideChar(CP_ACP, 0, p, len, nullptr, 0);
    }

    if (need <= 0)
        return std::wstring();

    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(cp, flags, p, len, &out[0], need);
    return out;
}

// ---------------------------------------------------------------------------
// Parse "section / key=value" text, keeping duplicate keys
// ---------------------------------------------------------------------------
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

static void ParseInto(const std::wstring& text, std::vector<Entry>& out)
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
            }
            continue;
        }

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;

        Entry e;
        e.section = section;
        e.key     = line.substr(0, eq);
        e.value   = line.substr(eq + 1);
        Trim(e.key);
        Trim(e.value);

        if (!e.key.empty())
            out.push_back(e);
    }
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
void Load(HMODULE self)
{
    if (g_loaded)
        return;

    g_loaded = true;

    std::string bytes;

    if (LoadResourceBytes(self, RES_EDITOR_INI, bytes))
    {
        g_editorIni = bytes;
        qeclog::Write(L"[cfg] 编辑器配置已加载：assets\\FinalAlert.ini（%u 字节）",
                      static_cast<unsigned>(bytes.size()));
    }
    else
    {
        qeclog::Write(L"[cfg] !! 资源里没有 FinalAlert.ini —— 编辑器会读到空配置 ✗（检查 FA2Entropy.rc）");
    }

    std::string dir;

    if (LoadResourceBytes(self, RES_DIRECTIVES, dir))
    {
        ParseInto(WideFromBytes(dir), g_items);
        g_haveDirectives = true;
        qeclog::Write(L"[cfg] 行为配置已加载：assets\\FA2Entropy.ini（%u 项）",
                      static_cast<unsigned>(g_items.size()));
    }
    else
    {
        qeclog::Write(L"[cfg] !! 资源里没有 FA2Entropy.ini —— 全部用内置默认值（能用，但改配置不生效）");
    }

    // Log the key entries on one line for quick verification after a config change.
    qeclog::Write(L"[cfg]   宿主=%s  影子=%s\\%s  锁定的ini=%s",
                  HostExe().c_str(), ShadowDir().c_str(),
                  ShadowFile().c_str(), LockedIni().c_str());

    // Warn on empty lists, usually a missing or misspelled ini entry.
    if (FixList().empty())
        qeclog::Write(L"[cfg] ⚠ [Include] Fix= 一条都没有：QEC 配置不会排到 include 最后 ✗");

    if (DropList().empty())
        qeclog::Write(L"[cfg] ⚠ [Include] Drop= 一条都没有：没有文件被拦截");

    if (ExtractList().empty())
        qeclog::Write(L"[cfg] ⚠ [Extract] 一条都没有：不会从 DLL 里释放 QEC 配置 ✗");
}

// Raw bytes of the editor config (assets\FinalAlert.ini), used by the shadow config
const std::string& EditorIniBytes()
{
    return g_editorIni;
}

// ---------------------------------------------------------------------------
// Value lookup
// ---------------------------------------------------------------------------
static bool SameName(const std::wstring& a, const wchar_t* b)
{
    return (b != nullptr) && (_wcsicmp(a.c_str(), b) == 0);
}

static const Entry* Find(const wchar_t* section, const wchar_t* key)
{
    if (section == nullptr || key == nullptr)
        return nullptr;

    for (const Entry& e : g_items)
    {
        if (SameName(e.section, section) && SameName(e.key, key))
            return &e;
    }
    return nullptr;
}

std::wstring S(const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    const Entry* e = Find(section, key);
    if (e != nullptr)
        return e->value;
    return (def != nullptr) ? std::wstring(def) : std::wstring();
}

int I(const wchar_t* section, const wchar_t* key, int def)
{
    const Entry* e = Find(section, key);
    if (e == nullptr)
        return def;
    return static_cast<int>(wcstol(e->value.c_str(), nullptr, 0));
}

unsigned U(const wchar_t* section, const wchar_t* key, unsigned def)
{
    const Entry* e = Find(section, key);
    if (e == nullptr)
        return def;
    return static_cast<unsigned>(wcstoul(e->value.c_str(), nullptr, 0));  // accepts 0x prefix
}

bool B(const wchar_t* section, const wchar_t* key, bool def)
{
    const Entry* e = Find(section, key);
    if (e == nullptr)
        return def;

    std::wstring v = e->value;
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<wchar_t>(towlower(v[i]));

    if (v == L"1" || v == L"yes" || v == L"true" || v == L"on")
        return true;
    if (v == L"0" || v == L"no" || v == L"false" || v == L"off")
        return false;
    return def;
}

std::vector<std::wstring> All(const wchar_t* section, const wchar_t* key)
{
    std::vector<std::wstring> out;

    for (const Entry& e : g_items)
    {
        if (SameName(e.section, section) && SameName(e.key, key))
            out.push_back(e.value);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Combined lists
// ---------------------------------------------------------------------------
std::vector<FixItem> FixList()
{
    std::vector<FixItem> out;

    for (const Entry& e : g_items)
    {
        if (!SameName(e.section, L"Include") || !SameName(e.key, L"Fix"))
            continue;

        const size_t eq = e.value.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
            continue;

        FixItem it;
        it.file  = e.value.substr(0, eq);
        it.entry = e.value.substr(eq + 1);
        Trim(it.file);
        Trim(it.entry);

        if (!it.file.empty() && !it.entry.empty())
            out.push_back(it);
    }

    if (out.empty() && !g_haveDirectives)      // no resource at all -> use built-in defaults
    {
        for (const auto& d : DEF_FIX)
        {
            FixItem it;
            it.file  = d[0];
            it.entry = d[1];
            out.push_back(it);
        }
    }

    return out;
}

std::vector<std::wstring> DropList()
{
    std::vector<std::wstring> out = All(L"Include", L"Drop");

    if (out.empty() && !g_haveDirectives)
    {
        for (const wchar_t* d : DEF_DROP)
            out.push_back(d);
    }

    return out;
}

std::vector<ExtractItem> ExtractList()
{
    std::vector<ExtractItem> out;

    for (const Entry& e : g_items)
    {
        if (!SameName(e.section, L"Extract"))
            continue;

        const int id = static_cast<int>(wcstol(e.key.c_str(), nullptr, 10));
        if (id <= 0 || e.value.empty())
            continue;

        ExtractItem it;
        it.resId = id;
        it.name  = e.value;
        out.push_back(it);
    }

    if (out.empty() && !g_haveDirectives)
    {
        ExtractItem a; a.resId = 101; a.name = L"qec01.dat"; out.push_back(a);
        ExtractItem b; b.resId = 102; b.name = L"qec02.dat"; out.push_back(b);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Common entries
// ---------------------------------------------------------------------------
std::wstring HostExe()       { return S(L"Host",   L"Exe",       DEF_HOST_EXE); }
unsigned     HostTimeStamp() { return U(L"Host",   L"TimeStamp", DEF_HOST_STAMP); }
std::wstring ShadowDir()     { return S(L"Shadow", L"Dir",       DEF_SHADOW_DIR); }
std::wstring ShadowFile()    { return S(L"Shadow", L"File",      DEF_SHADOW_FILE); }
std::wstring LockedIni()     { return S(L"Lock",   L"Ini",       DEF_LOCKED_INI); }
std::wstring UserIni()       { return S(L"Lock",   L"UserIni",   DEF_USER_INI); }
std::wstring DataDir()       { return S(L"Paths",  L"DataDir",   DEF_INI_DIR); }
std::wstring LegacyDataDir() { return S(L"Paths",  L"LegacyDataDir", DEF_LEGACY_DIR); }

std::vector<std::wstring> DataFileList()
{
    std::vector<std::wstring> out = All(L"DataFiles", L"Move");

    if (out.empty() && !g_haveDirectives)
    {
        for (const wchar_t* d : DEF_DATA_FILES)
            out.push_back(d);
    }

    return out;
}

std::wstring OthersDir() { return S(L"Others", L"Dir", DEF_OTHERS_DIR); }

// Default is an empty list, so nothing is moved; the human-readable logs FA2sp.log and finalalert2log.txt stay in the root
std::vector<std::wstring> OtherFileList()
{
    return All(L"Others", L"Move");
}

} // namespace cfg
