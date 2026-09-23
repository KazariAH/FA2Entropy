// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// Minimal logger
//
//  · No log file by default (config [Log] Enabled=0)
//  · Enabled=1 writes FA2Entropy.log next to the DLL (UTF-8 with BOM, Notepad readable)
//  · Either way every line also goes to OutputDebugString, so DebugView shows it live
#pragma once

#include <windows.h>

namespace qeclog {

// Create the critical section (first call from DllMain; no file is created here)
void Init(HMODULE self);

// Decide on the log file once the config is read; buffered lines are flushed when it opens
void SetFileEnabled(HMODULE self, bool on);

void Write(const wchar_t* fmt, ...);
void Close();

} // namespace qeclog
