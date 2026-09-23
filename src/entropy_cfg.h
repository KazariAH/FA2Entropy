// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  Configuration loading.
//
//    assets\FinalAlert.ini   editor config actually read (language, resource paths ...)
//    assets\FA2Entropy.ini   DLL behaviour (which files to intercept, menus to remove ...)
//
//  Both are compiled into the DLL resources (see FA2Entropy.rc) and read only from
//  there at runtime, so they ship nowhere else: to change configuration, edit the
//  ini under assets\ and rebuild (see README.md).
// =============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace cfg {

// Read both configs into memory. Must be called once before anything else (start of DllMain).
void Load(HMODULE self);

// Raw bytes of the editor config (assets\FinalAlert.ini), used by the shadow config
const std::string& EditorIniBytes();

// ANSI / UTF-8 to wide characters (auto-detected, BOM skipped)
std::wstring WideFromBytes(const std::string& bytes);

// Read a binary blob from the DLL resources
bool LoadResourceBytes(HMODULE self, int id, std::string& out);

// ---- [FA2Entropy.ini] value lookup (section/key case-insensitive; def on miss) ----
std::wstring S(const wchar_t* section, const wchar_t* key, const wchar_t* def);
int          I(const wchar_t* section, const wchar_t* key, int def);
unsigned     U(const wchar_t* section, const wchar_t* key, unsigned def);
bool         B(const wchar_t* section, const wchar_t* key, bool def);

// A key written on several lines returns all values (e.g. multiple Drop= lines)
std::vector<std::wstring> All(const wchar_t* section, const wchar_t* key);

// ---- Combined lists, taken as written in the config ----
struct FixItem { std::wstring file; std::wstring entry; };      // e.g. Fix=FAData.ini=qec01.dat
struct ExtractItem { int resId; std::wstring name; };           // e.g. [Extract] 101=qec01.dat

std::vector<FixItem>      FixList();
std::vector<std::wstring> DropList();
std::vector<ExtractItem>  ExtractList();

// ---- Common entries (logging and shared use) ----
std::wstring HostExe();
unsigned     HostTimeStamp();
std::wstring ShadowDir();
std::wstring ShadowFile();
std::wstring LockedIni();
std::wstring UserIni();        // [Lock] UserIni=     store for the non-fixed keys of the locked ini
std::wstring DataDir();        // [Paths] DataDir=      subfolder for ini / data files
std::wstring LegacyDataDir();  // [Paths] LegacyDataDir= old directory name (merged in at startup)
std::vector<std::wstring> DataFileList();   // [DataFiles] Move=  data files to move there too
std::wstring OthersDir();      // [Others] Dir=        where to move leftover files
std::vector<std::wstring> OtherFileList();  // [Others] Move=       leftover file list

} // namespace cfg
