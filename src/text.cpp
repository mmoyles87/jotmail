// SPDX-License-Identifier: GPL-3.0-or-later
#include "text.hpp"

#include <algorithm>
#include <iterator>

namespace jotmail {

#include "tables.inc"

namespace {

template <size_t N>
bool in_ranges(const char32_t (&ranges)[N][2], char32_t c) {
    auto it = std::upper_bound(std::begin(ranges), std::end(ranges), c,
                               [](char32_t v, const char32_t (&r)[2]) { return v < r[0]; });
    return it != std::begin(ranges) && c <= (*std::prev(it))[1];
}

}  // namespace

std::u32string utf8_decode(std::string_view s) {
    // Each maximal invalid subpart becomes one U+FFFD, as Python's errors="replace" does.
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < n) {
        unsigned char b = byte(i);
        if (b < 0x80) {
            out.push_back(b);
            ++i;
            continue;
        }
        int len;
        unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
        char32_t c;
        if (b >= 0xC2 && b <= 0xDF) {
            len = 2, c = b & 0x1F;
        } else if (b >= 0xE0 && b <= 0xEF) {
            len = 3, c = b & 0x0F;
            if (b == 0xE0) lo = 0xA0;
            if (b == 0xED) hi = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            len = 4, c = b & 0x07;
            if (b == 0xF0) lo = 0x90;
            if (b == 0xF4) hi = 0x8F;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        int k = 1;
        for (; k < len && i + k < n; ++k) {
            unsigned char t = byte(i + k);
            if (k == 1 ? (t < lo || t > hi) : (t & 0xC0) != 0x80)
                break;
            c = (c << 6) | (t & 0x3F);
        }
        if (k == len) {
            out.push_back(c);
        } else {
            out.push_back(0xFFFD);
        }
        i += k;
    }
    return out;
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

std::string utf8_encode(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s)
        append_utf8(out, c);
    return out;
}

bool is_space(char32_t c) { return in_ranges(SPACE_RANGES, c); }
bool is_word(char32_t c) { return in_ranges(WORD_RANGES, c); }
bool is_digit(char32_t c) { return in_ranges(DIGIT_RANGES, c); }

std::u32string strip(std::u32string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a]))
        ++a;
    while (b > a && is_space(s[b - 1]))
        --b;
    return std::u32string(s.substr(a, b - a));
}

std::u32string rstrip(std::u32string_view s) {
    size_t b = s.size();
    while (b > 0 && is_space(s[b - 1]))
        --b;
    return std::u32string(s.substr(0, b));
}

bool is_blank(std::u32string_view s) {
    return std::all_of(s.begin(), s.end(), is_space);
}

std::u32string ascii_lower(std::u32string_view s) {
    std::u32string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return out;
}

std::u32string html_escape(std::u32string_view s, bool quote) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        switch (c) {
            case '&': out += U"&amp;"; break;
            case '<': out += U"&lt;"; break;
            case '>': out += U"&gt;"; break;
            case '"': out += quote ? U"&quot;" : U"\""; break;
            case '\'': out += quote ? U"&#x27;" : U"'"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::u32string U(std::string_view ascii) {
    return std::u32string(ascii.begin(), ascii.end());
}

// Named character references, for html.cpp.
const char* lookup_named_ref(std::string_view name) {
    auto it = std::lower_bound(std::begin(NAMED_REFS), std::end(NAMED_REFS), name,
                               [](const NamedRef& r, std::string_view n) { return std::string_view(r.name) < n; });
    return it != std::end(NAMED_REFS) && name == it->name ? it->utf8 : nullptr;
}

}  // namespace jotmail
