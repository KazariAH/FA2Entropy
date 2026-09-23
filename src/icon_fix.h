// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  Replace the editor window icon with our own (legacyicon)
//
//  Why: the taskbar button uses the window icon (WM_SETICON / ICON_BIG) and the
//  title bar uses ICON_SMALL, falling back to the window class icon (GCLP_HICONSM).
//  The editor binary ships no icon of its own, so a renamed exe shows only the
//  system default.
//
//  How: the icon is embedded in FA2Entropy.dll (IDI_ENTROPY in FA2Entropy.rc);
//  when the watchdog finds the editor main window it sets WM_SETICON and the
//  window class icons once.
// =============================================================================

#pragma once

#include <windows.h>

namespace qecicon {

// Remember our own module handle (called from DllMain)
void Install(HMODULE self);

// Set the icon on a window (once per window); true = it was set this time
bool ApplyOnce(HWND hwnd);

} // namespace qecicon
