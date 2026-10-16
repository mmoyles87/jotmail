// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace jotmail {

struct ClipboardImage {
    std::string type;  // image/png, ...
    std::string bytes;
};

// The image on the clipboard: through `command` when set (it prints the image), else
// wl-paste (Wayland), xclip (X11) or pngpaste (macOS). Throws with a readable message when
// there is no image or no way to read the clipboard.
ClipboardImage clipboard_image(const std::string& command = "");

}  // namespace jotmail
