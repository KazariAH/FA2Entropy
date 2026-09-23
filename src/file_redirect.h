// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// FinalAlert.ini shadow copy + file redirection
#pragma once

#include <windows.h>

#include <string>

namespace qecfile {

// Build/refresh the shadow config and redirect the editor's FinalAlert.ini access to it
bool Install(HMODULE self);
void Uninstall();

// Full path of the shadow config file; empty before Install. The ini interception reads it
// as a fallback, because writes made through the editor's own file reader land there
// instead of in the preference store.
std::wstring ShadowConfigPath();

} // namespace qecfile
