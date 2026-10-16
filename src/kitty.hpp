// SPDX-License-Identifier: GPL-3.0-or-later
// Images in the terminal with the kitty graphics protocol's Unicode placeholders: the image is
// sent once, out of band, and drawn by ordinary text cells, so a TUI library needs no changes
// to show it and tmux can scroll and redraw it like text.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace jotmail::kitty {

struct Support {
    bool enabled = false;
    bool tmux = false;  // wrap commands in tmux passthrough
};

// mode is "auto", "kitty" or "off". "auto" enables images in kitty and Ghostty, and in tmux
// when the outer terminal is one of those and allow-passthrough is on.
Support detect(const std::string& mode);

// A row of placeholder cells: the text for row `row` of an image `cols` cells wide. Draw it with
// the foreground colour from id_color().
std::string placeholder_row(int row, int cols);
inline uint32_t id_color(uint32_t id) { return id & 0xFFFFFF; }

struct Placed {
    uint32_t id;
    int cols, rows;
};

class Images {
public:
    explicit Images(Support s);
    ~Images();  // deletes the images it sent

    // Sends the image (once) and places it to fit in max_cols x max_rows cells, keeping its
    // aspect ratio. `key` identifies the image across calls; `type` is its MIME type.
    // Nothing if images are off or the format cannot be decoded.
    std::optional<Placed> place(const std::string& key, const std::string& type, const std::string& bytes,
                                int max_cols, int max_rows);

    bool enabled() const { return support_.enabled; }

private:
    struct Sent {
        uint32_t id;
        int width, height;  // pixels
        int cols = 0, rows = 0;
        bool ok;
    };
    Support support_;
    uint32_t next_id_;
    std::map<std::string, Sent> sent_;

    void write(const std::string& command);  // one graphics command, wrapped for tmux if needed
};

}  // namespace jotmail::kitty
