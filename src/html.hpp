// SPDX-License-Identifier: GPL-3.0-or-later
// An HTML tokenizer with the same behaviour as CPython 3.13's html.parser.HTMLParser
// (convert_charrefs=True), including how it recovers from malformed markup.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jotmail {

// An attribute without a value (<a href>) has no value, as in Python.
using HtmlAttrs = std::vector<std::pair<std::u32string, std::optional<std::u32string>>>;

class HtmlHandler {
public:
    virtual ~HtmlHandler() = default;
    virtual void start_tag(const std::u32string& tag, const HtmlAttrs& attrs) {}
    virtual void end_tag(const std::u32string& tag) {}
    virtual void data(const std::u32string& text) {}
};

// Feed the whole document and close, as HTMLParser.feed() followed by close().
void parse_html(std::u32string_view doc, HtmlHandler& handler);

// html.unescape()
std::u32string html_unescape(std::u32string_view s);

}  // namespace jotmail
