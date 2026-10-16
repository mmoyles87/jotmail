// SPDX-License-Identifier: GPL-3.0-or-later
#include "mime.hpp"

#include <algorithm>
#include <functional>
#include <sstream>

#include <vmime/contentTypeField.hpp>
#include <vmime/vmime.hpp>

#include "text.hpp"

namespace jotmail {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string content_type(const vmime::header& h) {
    if (!h.hasField(vmime::fields::CONTENT_TYPE))
        return "text/plain";
    auto mt = h.ContentType()->getValue<vmime::mediaType>();
    return lower(mt->getType() + "/" + mt->getSubType());
}

std::string decoded_text(const vmime::bodyPart& part) {
    const auto& h = *part.getHeader();
    std::ostringstream raw;
    vmime::utility::outputStreamAdapter os(raw);
    part.getBody()->getContents()->extractRaw(os);

    std::string cte;
    if (h.hasField(vmime::fields::CONTENT_TRANSFER_ENCODING))
        cte = lower(h.ContentTransferEncoding()->getValue<vmime::encoding>()->getName());
    std::string bytes = raw.str();
    if (cte == "quoted-printable")
        bytes = decode_quoted_printable(bytes);
    else if (cte == "base64")
        bytes = decode_base64(bytes);

    std::string charset = "ascii";
    if (h.hasField(vmime::fields::CONTENT_TYPE)) {
        auto ct = vmime::dynamicCast<const vmime::contentTypeField>(h.ContentType());
        if (ct->hasParameter("charset"))
            charset = ct->getCharset().getName();
    }
    return to_utf8(bytes, charset);
}

}  // namespace

std::string note_html(std::string_view raw) {
    vmime::message msg;
    msg.parse(vmime::string(raw));
    std::function<const vmime::bodyPart*(const vmime::bodyPart&)> walk = [&](const vmime::bodyPart& p) -> const vmime::bodyPart* {
        if (content_type(*p.getHeader()) == "text/html")
            return &p;
        for (const auto& sub : p.getBody()->getPartList())
            if (auto hit = walk(*sub))
                return hit;
        return nullptr;
    };
    const vmime::bodyPart* html = walk(msg);
    return html ? decoded_text(*html) : std::string();
}

std::string decode_quoted_printable(std::string_view s) {
    std::string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] != '=') {
            out.push_back(s[i++]);
            continue;
        }
        if (++i >= n)
            break;
        if (s[i] == '\n' || s[i] == '\r') {  // soft line break: skip to past the newline
            while (i < n && s[i] != '\n')
                ++i;
            if (i < n)
                ++i;
        } else if (s[i] == '=') {
            out.push_back('=');
            ++i;
        } else if (i + 1 < n && hexval(s[i]) >= 0 && hexval(s[i + 1]) >= 0) {
            out.push_back(static_cast<char>(hexval(s[i]) << 4 | hexval(s[i + 1])));
            i += 2;
        } else {
            out.push_back('=');
        }
    }
    return out;
}

std::string decode_base64(std::string_view s) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    unsigned acc = 0;
    int bits = 0, quad = 0, pads = 0;
    for (char c : s) {
        if (c == '=') {
            if (quad >= 2 && quad + ++pads >= 4)
                break;  // padding that completes the quad ends the data
            continue;
        }
        int v = value(c);
        if (v < 0)
            continue;  // whitespace and other non-alphabet characters are ignored
        pads = 0;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        quad = (quad + 1) % 4;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    if (quad == 1)
        return std::string(s);  // an impossible length: Python hands back the input
    return out;
}

std::string to_utf8(std::string_view bytes, std::string charset) {
    charset = lower(charset);
    if (charset == "utf-8" || charset == "utf8")
        return utf8_encode(utf8_decode(bytes));
    if (charset == "us-ascii" || charset == "ascii" || charset == "646") {
        std::string out;
        for (unsigned char c : bytes) {
            if (c < 0x80)
                out.push_back(static_cast<char>(c));
            else
                append_utf8(out, 0xFFFD);
        }
        return out;
    }
    if (charset == "iso-8859-1" || charset == "latin-1" || charset == "latin1" || charset == "l1") {
        std::string out;
        for (unsigned char c : bytes)
            append_utf8(out, c);
        return out;
    }
    try {
        std::string out;
        vmime::charset::convert(std::string(bytes), out, vmime::charset(charset), vmime::charset(vmime::charsets::UTF_8));
        return out;
    } catch (const vmime::exception&) {
        return utf8_encode(utf8_decode(bytes));
    }
}

}  // namespace jotmail
