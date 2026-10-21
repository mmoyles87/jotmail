// SPDX-License-Identifier: GPL-3.0-or-later
#include "clipboard.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <sys/wait.h>

namespace jotmail {

namespace {

// The command's standard output, or nothing if it failed.
std::optional<std::string> capture(const std::string& cmd) {
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p)
        return std::nullopt;
    std::string out;
    char buf[65536];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        out.append(buf, n);
    int status = pclose(p);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::nullopt;
    return out;
}

bool have(const char* tool) {
    return capture(std::string("command -v ") + tool).has_value();
}

std::string sniff(const std::string& b) {
    if (b.starts_with("\x89PNG")) return "image/png";
    if (b.starts_with("\xFF\xD8\xFF")) return "image/jpeg";
    if (b.starts_with("GIF8")) return "image/gif";
    if (b.size() > 12 && b.starts_with("RIFF") && b.compare(8, 4, "WEBP") == 0) return "image/webp";
    return "";
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The image type to ask for from a list of offered types, in this order of preference. Only
// these exact names are used: the list comes from whatever program owns the clipboard, and the
// chosen name goes into a command line.
std::string pick(const std::string& types) {
    static const char* wanted[] = {"image/png", "image/jpeg", "image/webp", "image/gif"};
    std::vector<std::string> offered;
    std::istringstream in(types);
    for (std::string t; std::getline(in, t);) {
        while (!t.empty() && (t.back() == '\r' || t.back() == ' '))
            t.pop_back();
        offered.push_back(t);
    }
    for (const char* w : wanted)
        for (auto& t : offered)
            if (t == w)
                return t;
    return "";
}

ClipboardImage checked(std::optional<std::string> bytes, const std::string& how) {
    if (!bytes || bytes->empty())
        throw std::runtime_error("could not read the clipboard with " + how);
    std::string type = sniff(*bytes);
    if (type.empty())
        throw std::runtime_error("the clipboard does not hold a PNG, JPEG, GIF or WebP image");
    return {type, std::move(*bytes)};
}

// WAYLAND_DISPLAY, or the compositor's socket when tmux has not passed the variable on.
std::string wayland_display() {
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w)
        return w;
    const char* run = std::getenv("XDG_RUNTIME_DIR");
    if (!run)
        return "";
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(run, ec)) {
        std::string name = e.path().filename().string();
        if (name.starts_with("wayland-") && !name.ends_with(".lock") && e.is_socket(ec))
            return name;
    }
    return "";
}

}  // namespace

ClipboardImage clipboard_image(const std::string& command) {
    if (!command.empty())
        return checked(capture(command), "paste_command");

    if (std::string wl = wayland_display(); !wl.empty() && have("wl-paste")) {
        std::string env = "WAYLAND_DISPLAY=" + shell_quote(wl) + " ";
        auto types = capture(env + "wl-paste --list-types");
        if (types) {
            std::string type = pick(*types);
            if (type.empty())
                throw std::runtime_error("the clipboard holds no image");
            return checked(capture(env + "wl-paste --no-newline --type " + shell_quote(type)), "wl-paste");
        }
    }
    if (const char* d = std::getenv("DISPLAY"); d && *d && have("xclip")) {
        auto types = capture("xclip -selection clipboard -t TARGETS -o");
        if (types) {
            std::string type = pick(*types);
            if (type.empty())
                throw std::runtime_error("the clipboard holds no image");
            return checked(capture("xclip -selection clipboard -t " + shell_quote(type) + " -o"), "xclip");
        }
    }
    if (have("pngpaste"))
        return checked(capture("pngpaste -"), "pngpaste");
    throw std::runtime_error("no way to read the clipboard: install wl-clipboard, xclip or pngpaste, or set paste_command");
}

}  // namespace jotmail
