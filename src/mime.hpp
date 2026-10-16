// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>
#include <optional>
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

// The parts of a note's headers jotmail uses; values are decoded to UTF-8, missing ones empty.
struct NoteHeaders {
    std::string subject;
    std::string date;          // as written
    std::string created;       // X-Mail-Created-Date as written
    std::string uuid;          // X-Universally-Unique-Identifier, trimmed
    std::optional<std::time_t> date_value;
    bool multipart = false;
};
NoteHeaders note_headers(std::string_view raw);

// An attachment found by its Content-Id, transfer-decoded.
struct Attachment {
    std::string type;  // e.g. image/png
    std::string bytes;
};
std::optional<Attachment> find_attachment(std::string_view raw, std::string_view cid);

// Quoted-printable with CRLF line breaks and lines of at most 76 characters (RFC 2045).
std::string encode_quoted_printable(std::string_view s);

// A Subject value: plain when it is short printable ASCII, else RFC 2047 encoded words.
std::string encode_subject(std::string_view utf8);

// RFC 5322 date in local time, as Python's email.utils.formatdate(localtime=True).
std::string format_date(std::time_t t);

}  // namespace jotmail
