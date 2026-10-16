// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace jotmail {

// Opens `text` in $VISUAL, $EDITOR or vi (run through /bin/sh, so "code -w" works) and
// returns the edited text, or nothing if it was left unchanged.
std::optional<std::string> edit_text(const std::string& text);

// Keeps text that could not be saved in $XDG_STATE_HOME/jotmail/ and returns the file's path.
std::filesystem::path keep_unsaved(const std::string& text);

}  // namespace jotmail
