// SPDX-License-Identifier: GPL-3.0-or-later
#include "html.hpp"

#include <algorithm>

#include "text.hpp"

namespace jotmail {

const char* lookup_named_ref(std::string_view name);  // text.cpp

namespace {

constexpr size_t npos = std::u32string_view::npos;

bool is_alpha(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_hex(char32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_dec(char32_t c) { return c >= '0' && c <= '9'; }

// [\t\n\r\f ] -- the tokenizer's whitespace, which is narrower than Python's \s
bool is_tws(char32_t c) { return c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ' '; }

bool one_of(char32_t c, std::u32string_view set) { return set.find(c) != npos; }

bool starts_with(std::u32string_view s, size_t i, std::u32string_view p) {
    return s.substr(i, p.size()) == p;
}

bool starts_with_ci(std::u32string_view s, size_t i, std::u32string_view p) {
    if (i + p.size() > s.size())
        return false;
    for (size_t k = 0; k < p.size(); ++k) {
        char32_t a = s[i + k], b = p[k];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b)
            return false;
    }
    return true;
}

// --- character references -----------------------------------------------------

const char32_t WINDOWS_1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
};

bool invalid_codepoint(unsigned long c) {
    return (c >= 0x1 && c <= 0x8) || c == 0xB || (c >= 0xE && c <= 0x1F) || (c >= 0x7F && c <= 0x9F) ||
           (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE;
}

// _replace_charref for "#123", "#x7b", with or without the ';'
std::u32string numeric_ref(std::u32string_view s) {
    bool hex = s.size() > 1 && (s[1] == 'x' || s[1] == 'X');
    unsigned long num = 0;
    bool big = false;
    for (size_t k = hex ? 2 : 1; k < s.size() && s[k] != ';'; ++k) {
        unsigned d = is_dec(s[k]) ? s[k] - '0' : (s[k] | 0x20) - 'a' + 10;
        num = num * (hex ? 16 : 10) + d;
        if (num > 0x10FFFF)
            big = true, num = 0x110000;
    }
    if (big || num > 0x10FFFF || (num >= 0xD800 && num <= 0xDFFF))
        return U"�";
    if (num == 0)
        return U"�";
    if (num == 0x0D)
        return U"\r";
    if (num >= 0x80 && num <= 0x9F)
        return std::u32string(1, WINDOWS_1252[num - 0x80]);
    if (invalid_codepoint(num))
        return U"";
    return std::u32string(1, static_cast<char32_t>(num));
}

std::optional<std::u32string> named(std::u32string_view name) {
    std::string key;
    for (char32_t c : name) {
        if (c >= 0x80)
            return std::nullopt;  // every entity name is ASCII
        key.push_back(static_cast<char>(c));
    }
    if (const char* v = lookup_named_ref(key))
        return utf8_decode(v);
    return std::nullopt;
}

// _replace_charref for a named reference s (the text after '&')
std::u32string named_ref(std::u32string_view s) {
    if (auto v = named(s))
        return *v;
    for (size_t x = s.size() - 1; x > 1; --x)  // the longest legacy name that prefixes s
        if (auto v = named(s.substr(0, x)))
            return *v + std::u32string(s.substr(x));
    return U"&" + std::u32string(s);
}

}  // namespace

std::u32string html_unescape(std::u32string_view s) {
    if (s.find('&') == npos)
        return std::u32string(s);
    // _charref = &(#[0-9]+;?|#[xX][0-9a-fA-F]+;?|[^\t\n\f <&#;]{1,32};?)
    std::u32string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        size_t j = i + 1;
        if (j < n && s[j] == '#') {
            size_t k = j + 1;
            bool hex = k < n && (s[k] == 'x' || s[k] == 'X');
            size_t d = hex ? k + 1 : k;
            size_t e = d;
            while (e < n && (hex ? is_hex(s[e]) : is_dec(s[e])))
                ++e;
            if (e > d) {
                if (e < n && s[e] == ';')
                    ++e;
                out += numeric_ref(s.substr(j, e - j));
                i = e;
                continue;
            }
            // "&#" with no digits: the named alternative cannot start with '#', so no match here
            out.push_back('&');
            ++i;
            continue;
        }
        size_t e = j;
        while (e < n && e - j < 32 && !one_of(s[e], U"\t\n\f <&#;"))
            ++e;
        if (e == j) {
            out.push_back('&');
            ++i;
            continue;
        }
        if (e < n && s[e] == ';')
            ++e;
        out += named_ref(s.substr(j, e - j));
        i = e;
    }
    return out;
}

namespace {

// attr_charref = &(#[0-9]+|#[xX][0-9a-fA-F]+|[a-zA-Z][a-zA-Z0-9]*)[;=]?
std::u32string unescape_attr(std::u32string_view s) {
    std::u32string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        size_t j = i + 1, e = j;
        bool numeric = false;
        if (j < n && s[j] == '#') {
            size_t k = j + 1;
            bool hex = k < n && (s[k] == 'x' || s[k] == 'X');
            size_t d = hex ? k + 1 : k;
            e = d;
            while (e < n && (hex ? is_hex(s[e]) : is_dec(s[e])))
                ++e;
            if (e == d) {
                out.push_back(s[i++]);
                continue;
            }
            numeric = true;
        } else if (j < n && is_alpha(s[j])) {
            e = j + 1;
            while (e < n && (is_alpha(s[e]) || is_dec(s[e])))
                ++e;
        } else {
            out.push_back(s[i++]);
            continue;
        }
        if (e < n && (s[e] == ';' || s[e] == '='))
            ++e;
        std::u32string_view ref = s.substr(i, e - i);
        if (numeric || (ref.back() != '=' && named(ref.substr(1))))
            out += html_unescape(ref);
        else
            out += ref;
        i = e;
    }
    return out;
}

// --- tokenizer -------------------------------------------------------------------

struct AttrScan {
    size_t name_begin, name_end;
    bool has_value;
    size_t value_begin, value_end;  // the value as written, quotes included
    size_t end;                     // just past the attribute
};

// One attribute from position q, per attrfind_tolerant / locatetagend:
//   (?<=['"\t\n\r\f /])[^\t\n\r\f />][^\t\n\r\f /=>]*
//   ([\t\n\r\f ]*=[\t\n\r\f ]*('[^']*'|"[^"]*"|(?!['"])[^>\t\n\r\f ]*))?
std::optional<AttrScan> scan_attr(std::u32string_view s, size_t q) {
    size_t n = s.size();
    if (q == 0 || q >= n || !one_of(s[q - 1], U"'\"\t\n\r\f /") || one_of(s[q], U"\t\n\r\f />"))
        return std::nullopt;
    AttrScan a{q, q + 1, false, 0, 0, 0};
    while (a.name_end < n && !one_of(s[a.name_end], U"\t\n\r\f /=>"))
        ++a.name_end;
    a.end = a.name_end;

    size_t t = a.name_end;
    while (t < n && is_tws(s[t]))
        ++t;
    if (t < n && s[t] == '=') {
        size_t after_eq = t + 1;
        t = after_eq;
        while (t < n && is_tws(s[t]))
            ++t;
        if (t < n && (s[t] == '\'' || s[t] == '"')) {
            size_t close = s.find(s[t], t + 1);
            if (close != npos) {
                a.has_value = true, a.value_begin = t, a.value_end = close + 1, a.end = close + 1;
            } else if (t > after_eq) {
                // the regex gives back one whitespace character and takes an empty bare value there
                a.has_value = true, a.value_begin = a.value_end = a.end = t - 1;
            }
        } else {
            size_t e = t;
            while (e < n && !one_of(s[e], U">\t\n\r\f "))
                ++e;
            a.has_value = true, a.value_begin = t, a.value_end = e, a.end = e;
        }
    }
    return a;
}

// locatetagend, from the first letter of the tag name; returns the end of the match
size_t locate_tag_end(std::u32string_view s, size_t p) {
    size_t n = s.size(), q = p + 1;
    while (q < n && !one_of(s[q], U"\t\n\r\f />"))
        ++q;
    while (q < n && one_of(s[q], U"\t\n\r\f /"))
        ++q;
    while (auto a = scan_attr(s, q)) {
        q = a->end;
        while (q < n && one_of(s[q], U"\t\n\r\f /"))
            ++q;
    }
    if (q < n && s[q] == '>')
        ++q;
    return q;
}

// (?:[\t\n\r\f ]|/(?!>))*
size_t skip_tag_space(std::u32string_view s, size_t q) {
    while (q < s.size() && (is_tws(s[q]) || (s[q] == '/' && !(q + 1 < s.size() && s[q + 1] == '>'))))
        ++q;
    return q;
}

// tagfind_tolerant: the tag name at p and the position after it and its trailing space
std::pair<std::u32string, size_t> tag_name(std::u32string_view s, size_t p) {
    size_t q = p + 1;
    while (q < s.size() && !one_of(s[q], U"\t\n\r\f />"))
        ++q;
    return {ascii_lower(s.substr(p, q - p)), skip_tag_space(s, q)};
}

class Tokenizer {
public:
    Tokenizer(std::u32string_view s, HtmlHandler& h) : s_(s), h_(h) {}

    void run() {
        size_t i = 0, n = s_.size();
        while (i < n) {
            size_t j;
            if (cdata_.empty()) {
                j = s_.find('<', i);
                if (j == npos)
                    j = n;
            } else {
                j = find_cdata_end(i);
                if (j == npos)
                    break;
            }
            if (i < j)
                emit(s_.substr(i, j - i));
            i = j;
            if (i == n)
                break;

            long k;
            if (i + 1 < n && is_alpha(s_[i + 1]))
                k = parse_start_tag(i);
            else if (starts_with(s_, i, U"</"))
                k = parse_end_tag(i);
            else if (starts_with(s_, i, U"<!--"))
                k = parse_comment(i);
            else if (starts_with(s_, i, U"<?"))
                k = find_after(i + 2, '>');
            else if (starts_with(s_, i, U"<!"))
                k = parse_declaration(i);
            else {
                h_.data(U"<");
                k = static_cast<long>(i + 1);
            }
            if (k < 0) {
                // Unfinished markup at the end of the document: dropped, except a bare "</".
                if (starts_with(s_, i, U"</") && i + 2 == n)
                    h_.data(U"</");
                k = static_cast<long>(n);
            }
            i = static_cast<size_t>(k);
        }
        if (i < n)
            emit(s_.substr(i));
    }

private:
    std::u32string_view s_;
    HtmlHandler& h_;
    std::u32string cdata_;  // the raw-text element we are inside, if any
    bool escapable_ = true;

    void emit(std::u32string_view text) {
        h_.data(escapable_ ? html_unescape(text) : std::u32string(text));
    }

    long find_after(size_t from, char32_t c) {
        size_t p = s_.find(c, from);
        return p == npos ? -1 : static_cast<long>(p + 1);
    }

    // </elem followed by [\t\n\r\f />], case-insensitively
    size_t find_cdata_end(size_t i) {
        if (cdata_ == U"plaintext")
            return s_.size();
        for (size_t p = s_.find(U"</", i); p != npos; p = s_.find(U"</", p + 1)) {
            size_t after = p + 2 + cdata_.size();
            if (starts_with_ci(s_, p + 2, cdata_) && after < s_.size() && one_of(s_[after], U"\t\n\r\f />"))
                return p;
        }
        return npos;
    }

    long parse_start_tag(size_t i) {
        size_t endpos = locate_tag_end(s_, i + 1);
        if (s_[endpos - 1] != '>')
            return -1;
        auto [tag, k] = tag_name(s_, i + 1);
        HtmlAttrs attrs;
        while (k < endpos) {
            auto a = scan_attr(s_, k);
            if (!a)
                break;
            std::optional<std::u32string> value;
            if (a->has_value) {
                std::u32string_view v = s_.substr(a->value_begin, a->value_end - a->value_begin);
                if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
                    v = v.substr(1, v.size() - 2);
                value = v.empty() ? std::u32string() : unescape_attr(v);
            }
            attrs.emplace_back(ascii_lower(s_.substr(a->name_begin, a->name_end - a->name_begin)), std::move(value));
            k = skip_tag_space(s_, a->end);
        }
        std::u32string end = strip(s_.substr(k, endpos - k));
        if (end != U">" && end != U"/>") {
            h_.data(std::u32string(s_.substr(i, endpos - i)));
            return static_cast<long>(endpos);
        }
        h_.start_tag(tag, attrs);
        if (end == U"/>") {
            h_.end_tag(tag);
        } else if (tag == U"script" || tag == U"style" || tag == U"xmp" || tag == U"iframe" || tag == U"noembed" ||
                   tag == U"noframes" || tag == U"plaintext") {
            cdata_ = tag, escapable_ = false;
        } else if (tag == U"textarea" || tag == U"title") {
            cdata_ = tag, escapable_ = true;
        }
        return static_cast<long>(endpos);
    }

    long parse_end_tag(size_t i) {
        if (s_.find('>', i + 2) == npos)
            return -1;
        if (!(i + 2 < s_.size() && is_alpha(s_[i + 2]))) {
            if (s_[i + 2] == '>')
                return static_cast<long>(i + 3);  // "</>" is ignored
            return find_after(i + 2, '>');        // a bogus comment
        }
        size_t j = locate_tag_end(s_, i + 2);
        if (s_[j - 1] != '>')
            return -1;
        h_.end_tag(tag_name(s_, i + 2).first);
        cdata_.clear(), escapable_ = true;
        return static_cast<long>(j);
    }

    long parse_comment(size_t i) {
        // --!?> anywhere after "<!--", else an abrupt "-?>" straight after it
        for (size_t p = s_.find(U"--", i + 4); p != npos; p = s_.find(U"--", p + 1)) {
            if (starts_with(s_, p, U"-->"))
                return static_cast<long>(p + 3);
            if (starts_with(s_, p, U"--!>"))
                return static_cast<long>(p + 4);
        }
        if (starts_with(s_, i + 4, U">"))
            return static_cast<long>(i + 5);
        if (starts_with(s_, i + 4, U"->"))
            return static_cast<long>(i + 6);
        return -1;
    }

    long parse_declaration(size_t i) {
        if (starts_with(s_, i, U"<![CDATA[")) {
            size_t j = s_.find(U"]]>", i + 9);
            return j == npos ? -1 : static_cast<long>(j + 3);
        }
        if (starts_with_ci(s_, i, U"<!doctype"))
            return find_after(i + 9, '>');
        return find_after(i + 2, '>');
    }
};

}  // namespace

void parse_html(std::u32string_view doc, HtmlHandler& handler) {
    Tokenizer(doc, handler).run();
}

}  // namespace jotmail
