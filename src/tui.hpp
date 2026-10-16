// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "config.hpp"

namespace jotmail {

// The note browser. `images` is the config's images setting. Returns the exit status.
int run_tui(const Account& account, const std::string& images);

}  // namespace jotmail
