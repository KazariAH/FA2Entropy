// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  [AUTO-GENERATED, do not edit] Address of the Syringe placeholder hook
//
//  The DLL carries a .syhks00 section (see syringe_section.cpp) that makes Syringe:
//    (1) claim this DLL, so no separate FA2Entropy.dll.inj is needed
//    (2) load the DLL in the editor process, where DllMain does the real work
//
//  Why the address sits in the PE header:
//    Syringe calls LoadLibrary in ascending hook address order (std::map ordering)
//    and FA2sp.dll has many hooks, the lowest at 0x004014D0. The placeholder hook
//    sits in zero padding inside the PE header, below every code address in the
//    image, so this DLL is always loaded first and DllMain can call
//    SetDllDirectoryW before FA2sp runs, keeping FA2sp's static dependencies
//    (lua.dll / libcrypto_1_1.dll / Lexilla.dll / CncVxlRenderText.dll) in a subfolder.
//
//    That padding is never executed, so the breakpoint has no effect.
//
//  A new editor version (new FA2Entropy.dat): recompute the address below and rebuild.
//  The committed value is correct for the editor this DLL targets.
//
//  Current editor: FA2Entropy.dat
//  ImageBase = 0x00400000   placeholder hook = 0x00400400
// =============================================================================

#pragma once

#define ENTROPY_HOOK_ADDR 0x00400400u   // zero padding in the PE header (below all code, never executed)
#define ENTROPY_HOOK_SIZE 5u            // never executed, so the length does not matter