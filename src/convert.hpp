// SPDX-License-Identifier: GPL-3.0-or-later
// Note HTML <-> Markdown, for the subset iOS writes: divs, h1-h3, b/i, links, lists, images.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace jotmail {

std::string html_to_md(std::string_view html);
std::string md_to_html(std::string_view md);

// A line's inline formatting, for display: runs of text with their style.
struct Span {
    std::string text;
    bool bold = false, italic = false, link = false;
};
std::vector<Span> inline_spans(std::string_view line);

// The first non-blank line without heading marks or markdown punctuation, at most 120 characters.
std::string title_of(std::string_view md);

}  // namespace jotmail
