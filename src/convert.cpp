// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert.hpp"

#include <optional>
#include <vector>

#include "html.hpp"
#include "text.hpp"

namespace jotmail {

namespace {

constexpr size_t npos = std::u32string::npos;

// --- HTML -> Markdown ----------------------------------------------------------

class ToMarkdown : public HtmlHandler {
public:
    std::vector<std::u32string> lines;

    void flush(bool force = false) {
        if (!cur_.empty() || force)
            lines.push_back(rstrip(cur_));
        cur_.clear();
    }

    void start_tag(const std::u32string& tag, const HtmlAttrs& attrs) override {
        auto attr = [&](const char32_t* name) -> std::optional<std::u32string> {
            std::optional<std::u32string> v = std::u32string();  // absent: ""
            for (auto& [k, val] : attrs)
                if (k == name)
                    v = val;  // the last one wins, as in dict(attrs); present without a value: none
            return v;
        };
        if (tag == U"h1" || tag == U"h2" || tag == U"h3") {
            flush();
            cur_ = std::u32string(tag[1] - '0', '#') + U" ";
        } else if (tag == U"b" || tag == U"strong") {
            emit(U"**");
        } else if (tag == U"i" || tag == U"em") {
            emit(U"*");
        } else if (tag == U"a") {
            href_ = attr(U"href");
            link_text_.clear();
        } else if (tag == U"ul" || tag == U"ol") {
            flush();
            lists_.push_back({tag == U"ol", 0});
        } else if (tag == U"li") {
            flush();
            if (!lists_.empty() && lists_.back().ordered) {
                ++lists_.back().count;
                cur_ = U(std::to_string(lists_.back().count)) + U". ";
            } else {
                cur_ = U"- ";
            }
        } else if (tag == U"object") {
            std::u32string data = attr(U"data").value_or(U"");
            if (data.starts_with(U"cid:"))
                data = data.substr(4);
            emit(U"[attachment:" + data + U"]");
            in_object_ = true;
        } else if (tag == U"br") {
            if (in_object_)
                return;
            if (cur_.empty())
                lines.emplace_back();
            else
                flush();
        }
    }

    void end_tag(const std::u32string& tag) override {
        if (tag == U"object") {
            in_object_ = false;
        } else if (tag == U"b" || tag == U"strong") {
            emit(U"**");
        } else if (tag == U"i" || tag == U"em") {
            emit(U"*");
        } else if (tag == U"a") {
            std::u32string text = strip(link_text_);
            std::u32string href = href_.value_or(U"");
            href_.reset();
            cur_ += (text.empty() || text == href) ? href : U"[" + text + U"](" + href + U")";
        } else if (tag == U"ul" || tag == U"ol") {
            flush();
            if (!lists_.empty())
                lists_.pop_back();
        } else if (tag == U"div" || tag == U"p" || tag == U"h1" || tag == U"h2" || tag == U"h3" || tag == U"li") {
            flush();
        }
    }

    void data(const std::u32string& text) override {
        std::u32string d = text;
        for (auto& c : d)
            if (c == 0xA0)
                c = ' ';
        if (!is_blank(d) || !cur_.empty()) {
            for (auto& c : d)
                if (c == '\n')
                    c = ' ';
            emit(d);
        }
    }

private:
    struct List {
        bool ordered;
        int count;
    };
    std::u32string cur_, link_text_;
    // None outside a link and for <a href> with no value, as in the Python; "" for <a> without href.
    std::optional<std::u32string> href_;
    std::vector<List> lists_;
    bool in_object_ = false;

    void emit(const std::u32string& s) {
        if (href_)
            link_text_ += s;
        else
            cur_ += s;
    }
};

bool ci_at(const std::u32string& s, size_t i, std::u32string_view p) {
    if (i + p.size() > s.size())
        return false;
    for (size_t k = 0; k < p.size(); ++k) {
        char32_t c = s[i + k];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != p[k])
            return false;
    }
    return true;
}

// re.search(r"<body[^>]*>(.*)</body>", src, re.S | re.I)
std::optional<std::u32string> body_of(const std::u32string& src) {
    size_t last = npos;
    for (size_t p = src.size() >= 7 ? src.size() - 7 + 1 : 0; p-- > 0;)
        if (ci_at(src, p, U"</body>")) {
            last = p;
            break;
        }
    if (last == npos)
        return std::nullopt;
    for (size_t p = 0; p + 5 <= src.size(); ++p) {
        if (!ci_at(src, p, U"<body"))
            continue;
        size_t gt = src.find('>', p + 5);
        if (gt == npos || gt + 1 > last)
            return std::nullopt;
        return src.substr(gt + 1, last - gt - 1);
    }
    return std::nullopt;
}

// --- Markdown -> HTML ----------------------------------------------------------

bool url_start(const std::u32string& s, size_t p, size_t& after) {
    if (s.compare(p, 4, U"http") != 0)
        return false;
    size_t q = p + 4;
    if (q < s.size() && s[q] == 's' && s.compare(q + 1, 3, U"://") == 0)
        q += 1;
    if (s.compare(q, 3, U"://") != 0)
        return false;
    after = q + 3;
    return true;
}

// One alternative of _INLINE matched at p.
struct InlineMatch {
    enum Kind { Link, Url, Bold, Italic } kind;
    std::u32string text, url;  // the visible text (inner text for Bold and Italic) and the link target
    size_t end;
};

std::optional<InlineMatch> inline_at(const std::u32string& t, size_t p) {
    size_t n = t.size();
    // \[(?P<lt>[^\]]+)\]\((?P<lu>https?://[^)\s]+)\)
    if (t[p] == '[') {
        size_t close = t.find(']', p + 1);
        size_t u;
        if (close != npos && close > p + 1 && close + 1 < n && t[close + 1] == '(' && url_start(t, close + 2, u)) {
            size_t e = u;
            while (e < n && t[e] != ')' && !is_space(t[e]))
                ++e;
            if (e > u && e < n && t[e] == ')')
                return InlineMatch{InlineMatch::Link, t.substr(p + 1, close - p - 1), t.substr(close + 2, e - close - 2), e + 1};
        }
    }
    // (?P<url>https?://[^\s<>\]\)]+)
    if (size_t u; t[p] == 'h' && url_start(t, p, u)) {
        size_t e = u;
        while (e < n && !is_space(t[e]) && t[e] != '<' && t[e] != '>' && t[e] != ']' && t[e] != ')')
            ++e;
        if (e > u)
            return InlineMatch{InlineMatch::Url, t.substr(p, e - p), t.substr(p, e - p), e};
    }
    // \*\*(?P<b>.+?)\*\*
    if (t.compare(p, 2, U"**") == 0) {
        for (size_t e = p + 3; e + 1 < n; ++e) {
            if (t[e - 1] == '\n')
                break;
            if (t[e] == '*' && t[e + 1] == '*')
                return InlineMatch{InlineMatch::Bold, t.substr(p + 2, e - p - 2), {}, e + 2};
        }
    }
    // (?<![*\w])\*(?P<i>[^*\s][^*]*?)\*(?!\*)
    if (t[p] == '*' && !(p > 0 && (t[p - 1] == '*' || is_word(t[p - 1]))) && p + 1 < n && t[p + 1] != '*' &&
        !is_space(t[p + 1])) {
        size_t e = p + 2;
        while (e < n && t[e] != '*')
            ++e;
        if (e < n && t[e] == '*' && !(e + 1 < n && t[e + 1] == '*'))
            return InlineMatch{InlineMatch::Italic, t.substr(p + 1, e - p - 1), {}, e + 1};
    }
    return std::nullopt;
}

std::u32string inline_md(const std::u32string& text) {
    std::u32string out;
    size_t pos = 0, p = 0;
    while (p < text.size()) {
        if (auto m = inline_at(text, p)) {
            out += html_escape(text.substr(pos, p - pos), false);
            switch (m->kind) {
                case InlineMatch::Link:
                    out += U"<a href=\"" + html_escape(m->url, true) + U"\">" + html_escape(m->text, false) + U"</a>";
                    break;
                case InlineMatch::Url:
                    out += U"<a href=\"" + html_escape(m->url, true) + U"\">" + html_escape(m->url, true) + U"</a>";
                    break;
                case InlineMatch::Bold: out += U"<b>" + inline_md(m->text) + U"</b>"; break;
                case InlineMatch::Italic: out += U"<i>" + inline_md(m->text) + U"</i>"; break;
            }
            pos = p = m->end;
        } else {
            ++p;
        }
    }
    out += html_escape(text.substr(pos), false);
    return out;
}

void inline_spans(const std::u32string& text, bool bold, bool italic, std::vector<Span>& out) {
    size_t pos = 0, p = 0;
    auto plain = [&](size_t a, size_t b) {
        if (b > a)
            out.push_back({utf8_encode(text.substr(a, b - a)), bold, italic, false, {}});
    };
    while (p < text.size()) {
        if (auto m = inline_at(text, p)) {
            plain(pos, p);
            switch (m->kind) {
                case InlineMatch::Link:
                case InlineMatch::Url: out.push_back({utf8_encode(m->text), bold, italic, true, utf8_encode(m->url)}); break;
                case InlineMatch::Bold: inline_spans(m->text, true, italic, out); break;
                case InlineMatch::Italic: inline_spans(m->text, bold, true, out); break;
            }
            pos = p = m->end;
        } else {
            ++p;
        }
    }
    plain(pos, text.size());
}

std::vector<std::u32string> split_lines(const std::u32string& s) {
    std::vector<std::u32string> out;
    size_t a = 0;
    for (size_t b; (b = s.find('\n', a)) != npos; a = b + 1)
        out.push_back(s.substr(a, b - a));
    out.push_back(s.substr(a));
    return out;
}

// ^\s*[-*]\s+(.*)  or  ^\s*\d+[.)]\s+(.*)
std::optional<std::u32string> list_item(const std::u32string& line, bool numbered) {
    size_t i = 0, n = line.size();
    while (i < n && is_space(line[i]))
        ++i;
    if (numbered) {
        size_t d = i;
        while (i < n && is_digit(line[i]))
            ++i;
        if (i == d || i >= n || (line[i] != '.' && line[i] != ')'))
            return std::nullopt;
    } else if (i >= n || (line[i] != '-' && line[i] != '*')) {
        return std::nullopt;
    }
    ++i;
    size_t s = i;
    while (i < n && is_space(line[i]))
        ++i;
    if (i == s)
        return std::nullopt;
    return line.substr(i);
}

}  // namespace

std::string html_to_md(std::string_view html) {
    std::u32string src = utf8_decode(html);
    ToMarkdown p;
    auto body = body_of(src);
    parse_html(body ? *body : src, p);
    p.flush();
    auto& lines = p.lines;
    while (!lines.empty() && is_blank(lines.back()))
        lines.pop_back();
    std::u32string out;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k)
            out += U"\n";
        out += lines[k];
    }
    return utf8_encode(out + U"\n");
}

std::string md_to_html(std::string_view md_utf8) {
    std::u32string md = utf8_decode(md_utf8);
    while (!md.empty() && md.back() == '\n')
        md.pop_back();
    std::vector<std::u32string> out;
    std::u32string lst;  // the open list: "ul", "ol" or none
    for (auto& line : split_lines(md)) {
        auto bullet = list_item(line, false);
        auto number = bullet ? std::nullopt : list_item(line, true);
        std::u32string kind = bullet ? U"ul" : number ? U"ol" : U"";
        if (kind != lst) {
            if (!lst.empty())
                out.push_back(U"</" + lst + U">");
            if (!kind.empty())
                out.push_back(U"<" + kind + U">");
            lst = kind;
        }
        if (!kind.empty()) {
            out.push_back(U"<li>" + inline_md(bullet ? *bullet : *number) + U"</li>");
            continue;
        }
        // ^\[attachment:([^\]\s]+)\]\s*$
        if (line.starts_with(U"[attachment:")) {
            size_t e = 12;
            while (e < line.size() && line[e] != ']' && !is_space(line[e]))
                ++e;
            if (e > 12 && e < line.size() && line[e] == ']' && is_blank(std::u32string_view(line).substr(e + 1))) {
                out.push_back(U"<div><object type=\"application/x-apple-msg-attachment\" data=\"cid:" +
                              html_escape(line.substr(12, e - 12), true) + U"\"><br></object><br></div>");
                continue;
            }
        }
        if (is_blank(line)) {
            out.push_back(U"<div><br></div>");
            continue;
        }
        // ^(#{1,3})\s+(.*)
        size_t h = 0;
        while (h < line.size() && h < 3 && line[h] == '#')
            ++h;
        if (h > 0 && h < line.size() && is_space(line[h])) {
            size_t s = h;
            while (s < line.size() && is_space(line[s]))
                ++s;
            std::u32string tag = U"h" + std::u32string(1, U'0' + h);
            out.push_back(U"<div><" + tag + U">" + inline_md(line.substr(s)) + U"</" + tag + U"></div>");
            continue;
        }
        out.push_back(U"<div>" + inline_md(line) + U"</div>");
    }
    if (!lst.empty())
        out.push_back(U"</" + lst + U">");

    std::u32string html = U"<html><head></head><body>";
    for (size_t k = 0; k < out.size(); ++k) {
        if (k)
            html += U"\n";
        html += out[k];
    }
    return utf8_encode(html + U"</body></html>");
}

std::vector<Span> inline_spans(std::string_view line) {
    std::vector<Span> out;
    inline_spans(utf8_decode(line), false, false, out);
    return out;
}

std::string title_of(std::string_view md_utf8) {
    std::u32string md = utf8_decode(md_utf8);
    for (auto& line : split_lines(md)) {
        size_t i = 0;
        if (!line.empty() && line[0] == '#') {
            while (i < line.size() && line[i] == '#')
                ++i;
            while (i < line.size() && is_space(line[i]))
                ++i;
        }
        std::u32string t = strip(std::u32string_view(line).substr(i));
        if (t.empty())
            continue;
        std::u32string kept;
        for (char32_t c : t)
            if (c != '*' && c != '_' && c != '`' && c != '[' && c != ']')
                kept.push_back(c);
        return utf8_encode(kept.substr(0, 120));
    }
    return "New Note";
}

}  // namespace jotmail
