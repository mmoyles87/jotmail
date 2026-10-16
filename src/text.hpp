// SPDX-License-Identifier: GPL-3.0-or-later
// Unicode text helpers that behave like Python's str methods and re classes.
#pragma once

#include <string>
#include <string_view>

namespace jotmail {

std::u32string utf8_decode(std::string_view s);  // invalid sequences become U+FFFD
std::string utf8_encode(std::u32string_view s);
void append_utf8(std::string& out, char32_t c);

bool is_space(char32_t c);  // Python's \s and str.isspace()
bool is_word(char32_t c);   // Python's \w
bool is_digit(char32_t c);  // Python's \d

std::u32string strip(std::u32string_view s);   // str.strip()
std::u32string rstrip(std::u32string_view s);  // str.rstrip()
bool is_blank(std::u32string_view s);          // not s.strip()

std::u32string ascii_lower(std::u32string_view s);

// html.escape(s, quote)
std::u32string html_escape(std::u32string_view s, bool quote);

std::u32string U(std::string_view ascii);  // widen an ASCII literal

}  // namespace jotmail
