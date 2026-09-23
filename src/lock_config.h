// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  Internal constants; this file normally needs no edits.
//
//  Editable configuration lives in assets\ as plain ini files:
//      assets\FinalAlert.ini    editor config actually read (language, resource paths ...)
//      assets\FA2Entropy.ini    DLL behaviour (intercepted files, removed menus, shadow name ...)
//      assets\Quantum01.ini     QEC config
//      assets\Quantum02.ini     QEC language
//
//  Only values that are pointless to edit or must be fixed at compile time live here:
//  the resource IDs (which must match FA2Entropy.rc) and the log / debug marker file
//  names (both next to the DLL).
// =============================================================================

#pragma once

// ---- DLL resource IDs (see FA2Entropy.rc) ----
#define RES_EDITOR_INI      100   // assets\FinalAlert.ini
#define RES_QEC_CONFIG      101   // assets\Quantum01.ini
#define RES_QEC_LANGUAGE    102   // assets\Quantum02.ini
#define RES_DIRECTIVES      103   // assets\FA2Entropy.ini
#define RES_APP_ICON        201   // launch\legacyicon.ico (set on the editor window)

// ---- Log / debug marker (both next to the DLL) ----
// Log: UTF-8 with BOM, readable directly in Notepad
#define ENTROPY_LOG_FILENAME      L"FA2Entropy.log"
// Log-only mode: an empty FA2Entropy.logonly file next to the DLL dumps the whole
// menu tree into the log without removing anything, to identify target menu items
#define ENTROPY_LOGONLY_MARKER    L"FA2Entropy.logonly"
// Trace mode: an empty FA2Entropy.trace file next to the DLL logs every file the editor
// opens for writing. Diagnostic only; it needs [Log] Enabled=1 to be visible.
#define ENTROPY_TRACE_MARKER      L"FA2Entropy.trace"
