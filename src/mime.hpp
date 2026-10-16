// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace jotmail {

// The first text/html part of a message (depth first, the message itself included),
// transfer-decoded and converted to UTF-8; empty if there is none.
std::string note_html(std::string_view raw);

// Transfer decodings with the same leniency as Python's email package.
std::string decode_quoted_printable(std::string_view s);  // binascii.a2b_qp
std::string decode_base64(std::string_view s);            // email._encoded_words.decode_b

// bytes.decode(charset, errors="replace"); unknown charsets fall back to UTF-8.
std::string to_utf8(std::string_view bytes, std::string charset);

}  // namespace jotmail
