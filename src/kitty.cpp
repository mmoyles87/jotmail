// SPDX-License-Identifier: GPL-3.0-or-later
#include "kitty.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#ifdef JOTMAIL_HAVE_WEBP
#include <webp/decode.h>
#endif

#include "text.hpp"

namespace jotmail::kitty {

namespace {

#include "diacritics.inc"

constexpr int MAX_CELLS = static_cast<int>(sizeof DIACRITICS / sizeof DIACRITICS[0]);
constexpr int MAX_PIXELS = 1600;
constexpr long long MAX_SOURCE_PIXELS = 50'000'000;  // larger images are not decoded (~200 MB as RGBA)  // larger decoded images are scaled down before sending

std::string run(const char* cmd) {
    std::string out;
    if (FILE* p = popen(cmd, "r")) {
        char buf[256];
        while (size_t n = fread(buf, 1, sizeof buf, p))
            out.append(buf, n);
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

std::string base64(const unsigned char* data, size_t n) {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out += B64[v >> 18 & 63];
        out += B64[v >> 12 & 63];
        out += i + 1 < n ? B64[v >> 6 & 63] : '=';
        out += i + 2 < n ? B64[v & 63] : '=';
    }
    return out;
}

// Pixel width / height of one cell; 0.5 when the terminal does not say.
double cell_aspect() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel && ws.ws_ypixel && ws.ws_col && ws.ws_row)
        return (static_cast<double>(ws.ws_xpixel) / ws.ws_col) / (static_cast<double>(ws.ws_ypixel) / ws.ws_row);
    return 0.5;
}

}  // namespace

Support detect(const std::string& mode) {
    Support s;
    if (mode == "off")
        return s;
    s.tmux = std::getenv("TMUX") != nullptr;
    if (mode == "kitty") {
        s.enabled = true;
        return s;
    }
    auto speaks_kitty = [](const std::string& name) { return contains(name, "kitty") || contains(name, "ghostty"); };
    if (s.tmux) {
        std::string info = run("tmux display -p '#{client_termname} #{allow-passthrough}' 2>/dev/null");
        bool passthrough = info.ends_with(" on") || info.ends_with(" all");
        s.enabled = speaks_kitty(info) && passthrough;
    } else {
        const char* term = std::getenv("TERM");
        const char* program = std::getenv("TERM_PROGRAM");
        s.enabled = (term && speaks_kitty(term)) || (program && speaks_kitty(program));
    }
    return s;
}

std::string placeholder_row(int row, int cols) {
    std::string out;
    row = std::clamp(row, 0, MAX_CELLS - 1);
    for (int c = 0; c < std::min(cols, MAX_CELLS); ++c) {
        append_utf8(out, 0x10EEEE);
        append_utf8(out, DIACRITICS[row]);
        append_utf8(out, DIACRITICS[c]);
    }
    return out;
}

Images::Images(Support s) : support_(s) {
    // Image ids are shared by everything in the terminal window (every tmux pane, too), so
    // start somewhere unlikely to collide.
    std::mt19937 rng{std::random_device{}()};
    next_id_ = 0x100000 + rng() % 0xE00000;
}

Images::~Images() {
    for (auto& [_, s] : sent_)
        if (s.ok)
            write("a=d,d=I,i=" + std::to_string(s.id) + ",q=2");
    std::cout << std::flush;
}

void Images::write(const std::string& command) {
    std::string seq = "\x1b_G" + command + "\x1b\\";
    if (support_.tmux) {
        std::string wrapped = "\x1bPtmux;";
        for (char c : seq)
            wrapped += c == '\x1b' ? std::string("\x1b\x1b") : std::string(1, c);
        seq = wrapped + "\x1b\\";
    }
    std::cout << seq;
}

std::optional<Placed> Images::place(const std::string& key, const std::string& type, const std::string& bytes,
                                    int max_cols, int max_rows) {
    if (!support_.enabled || max_cols < 1 || max_rows < 1)
        return std::nullopt;

    auto it = sent_.find(key);
    if (it == sent_.end()) {
        Sent s{next_id_++ & 0xFFFFFF, 0, 0, 0, 0, false};
        if (s.id == 0)
            s.id = next_id_++;
        std::string keys, payload;
        auto data = reinterpret_cast<const unsigned char*>(bytes.data());
        bool png = bytes.size() > 24 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0;
        long long png_w = png ? (long long)(data[16] << 24 | data[17] << 16 | data[18] << 8 | data[19]) : 0;
        long long png_h = png ? (long long)(data[20] << 24 | data[21] << 16 | data[22] << 8 | data[23]) : 0;
        if (png && std::max(png_w, png_h) <= MAX_PIXELS) {  // small enough to send as it is
            s.width = static_cast<int>(png_w), s.height = static_cast<int>(png_h);
            keys = "f=100";
            payload = bytes;
        } else {
            int w = 0, h = 0, n = 0;
            unsigned char* rgba = nullptr;
            bool from_stb = true;
            // The size first: decoding expands to 4 bytes a pixel before any resize, so a huge
            // image is refused rather than decoded.
            bool is_webp = type == "image/webp" || (bytes.size() > 12 && bytes.compare(8, 4, "WEBP") == 0);
            int iw = 0, ih = 0, in = 0;
#ifdef JOTMAIL_HAVE_WEBP
            if (is_webp && !WebPGetInfo(data, bytes.size(), &iw, &ih))
                iw = ih = 0;
#endif
            if (!is_webp && !stbi_info_from_memory(data, static_cast<int>(bytes.size()), &iw, &ih, &in))
                iw = ih = 0;
            bool too_big = static_cast<long long>(iw) * ih > MAX_SOURCE_PIXELS;
#ifdef JOTMAIL_HAVE_WEBP
            if (is_webp && !too_big && iw > 0) {
                rgba = WebPDecodeRGBA(data, bytes.size(), &w, &h);
                from_stb = false;
            }
#endif
            if (!rgba && from_stb && !too_big && iw > 0)
                rgba = stbi_load_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &n, 4);
            if (rgba) {
                std::vector<unsigned char> px(rgba, rgba + static_cast<size_t>(w) * h * 4);
                if (from_stb)
                    stbi_image_free(rgba);
#ifdef JOTMAIL_HAVE_WEBP
                else
                    WebPFree(rgba);
#endif
                if (std::max(w, h) > MAX_PIXELS) {
                    double k = static_cast<double>(MAX_PIXELS) / std::max(w, h);
                    int nw = std::max(1, static_cast<int>(w * k)), nh = std::max(1, static_cast<int>(h * k));
                    std::vector<unsigned char> small(static_cast<size_t>(nw) * nh * 4);
                    stbir_resize_uint8_srgb(px.data(), w, h, 0, small.data(), nw, nh, 0, STBIR_RGBA);
                    px.swap(small), w = nw, h = nh;
                }
                uLongf len = compressBound(px.size());
                std::string z(len, '\0');
                if (compress2(reinterpret_cast<Bytef*>(z.data()), &len, px.data(), px.size(), 6) == Z_OK) {
                    z.resize(len);
                    s.width = w, s.height = h;
                    keys = "f=32,s=" + std::to_string(w) + ",v=" + std::to_string(h) + ",o=z";
                    payload = std::move(z);
                }
            }
        }
        s.ok = !payload.empty() && s.width > 0 && s.height > 0;
        if (s.ok) {
            std::string b64 = base64(reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
            constexpr size_t CHUNK = 4096;
            for (size_t off = 0; off < b64.size(); off += CHUNK) {
                bool more = off + CHUNK < b64.size();
                std::string head = off == 0 ? "a=t,t=d,q=2,i=" + std::to_string(s.id) + "," + keys + "," : "";
                write(head + "m=" + (more ? "1" : "0") + ";" + b64.substr(off, CHUNK));
            }
        }
        it = sent_.emplace(key, s).first;
    }
    Sent& s = it->second;
    if (!s.ok)
        return std::nullopt;

    // Fit inside max_cols x max_rows, never wider than the image's own pixels.
    double aspect = cell_aspect();
    int cols = std::min({max_cols, MAX_CELLS, std::max(1, static_cast<int>(s.width * 1.0 / 8))});
    int rows = std::max(1, static_cast<int>(std::lround(cols * (static_cast<double>(s.height) / s.width) * aspect)));
    if (rows > max_rows || rows > MAX_CELLS) {
        rows = std::min(max_rows, MAX_CELLS);
        cols = std::max(1, static_cast<int>(std::lround(rows / ((static_cast<double>(s.height) / s.width) * aspect))));
        cols = std::min(cols, max_cols);
    }
    if (cols != s.cols || rows != s.rows) {
        write("a=p,U=1,q=2,i=" + std::to_string(s.id) + ",p=1,c=" + std::to_string(cols) + ",r=" + std::to_string(rows));
        s.cols = cols, s.rows = rows;
    }
    return Placed{s.id, cols, rows};
}

}  // namespace jotmail::kitty
