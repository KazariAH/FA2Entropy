// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 faucheux

// FinalAlert.ini read/write interception
#pragma once

#include <string>

namespace qecini {

// Install the hooks; true only if all succeeded (failures are logged).
bool Install();
void Uninstall();

// Merge the built-in config keys into an ini text (ANSI). Used for the shadow config
std::string MergeIntoIniText(const std::string& base);

// Merge the keys of another ini (the preference store, ANSI or UTF-16 bytes) into an
// ini text (ANSI). Keys already in "base" are replaced. Used for the shadow config.
std::string MergeTextIntoIniText(const std::string& base, const std::string& extra);

// Keys of "current" that are new or hold a different value than in "baseline", as an ini
// text (one section header per section). Working copies of the editor's data files are
// rebuilt on every start, so whatever the editor changed in them during a run has to be
// carried over into the preference store before they are thrown away.
std::string DiffIniText(const std::string& baseline, const std::string& current);

// Tidy up an ini text: one blank line between sections, no repeated or trailing blank
// lines. Used for the files a user can open.
std::string FormatIniText(const std::string& text);

// Remove the built-in config keys from an ini text (on shutdown, leaving no paths on disk)
std::string StripFromIniText(const std::string& text);
void Uninstall();

} // namespace qecini
