// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// =============================================================================
//  Makes Syringe claim this DLL (the .syhks00 section)
//
//  Syringe scans *.dll in the editor directory:
//    a .syhks00 section -> use the hook table inside it (this file)
//    otherwise a .inj file with the same name -> use the hook table in it
//    neither -> the DLL is never loaded
//
//  Syringe calls LoadLibrary only when a hook is hit, so at least one hook is
//  required for DllMain to run inside the editor.
//
//  This is one placeholder hook: address = host entry point (see syringe_hook.h),
//  function name = our exported QecBootstrap. The hook itself does nothing; all
//  work happens in DllMain when Syringe loads the DLL.
//
//  Result: the release package needs only the DLL, no FA2Entropy.dll.inj.
// =============================================================================

#include "syringe_hook.h"

// Section name and layout must match what Syringe expects (Syringe's include\Syringe.h):
//   __declspec(align(16)) struct hookdecl { unsigned int hookAddr; unsigned int hookSize; const char* hookName; };
// Syringe walks this section in sizeof(hookdecl) = 16 byte records.
#pragma section(".syhks00", read, write)

#pragma pack(push, 16)
__declspec(align(16)) struct SyringeHookDecl
{
    unsigned int hookAddr;
    unsigned int hookSize;
    const char*  hookName;
};
#pragma pack(pop)

namespace SyringeData
{
namespace Hooks
{
    // Address/size come from syringe_hook.h, generated from the editor executable
    __declspec(allocate(".syhks00")) __declspec(align(16))
    SyringeHookDecl _hk_QecBootstrap =
    {
        ENTROPY_HOOK_ADDR,
        ENTROPY_HOOK_SIZE,
        "QecBootstrap"
    };
}
}
