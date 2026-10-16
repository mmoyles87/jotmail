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

// --- note headers, attachments, encodings -----------------------------------------

namespace jotmail {

namespace {

// The unfolded value of the first `name` field in the top-level header block, if present.
std::optional<std::string> raw_header(std::string_view raw, std::string_view name) {
    size_t end = raw.find("\r\n\r\n");
    if (end == std::string_view::npos)
        end = raw.find("\n\n");
    std::string_view head = raw.substr(0, end);
    size_t pos = 0;
    while (pos < head.size()) {
        size_t eol = head.find('\n', pos);
        if (eol == std::string_view::npos)
            eol = head.size();
        std::string_view line = head.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos && colon == name.size() && lower(std::string(line.substr(0, colon))) == lower(std::string(name))) {
            std::string value(line.substr(colon + 1));
            size_t next = eol + 1;
            while (next < head.size() && (head[next] == ' ' || head[next] == '\t')) {  // continuation lines
                size_t e = head.find('\n', next);
                if (e == std::string_view::npos)
                    e = head.size();
                value += " ";
                value += head.substr(next, e - next);
                next = e + 1;
            }
            std::erase(value, '\r');
            size_t a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : value.substr(a, b - a + 1);
        }
        pos = eol + 1;
    }
    return std::nullopt;
}

}  // namespace

NoteHeaders note_headers(std::string_view raw) {
    NoteHeaders h;
    if (auto s = raw_header(raw, "Subject"))
        h.subject = vmime::text::decodeAndUnfold(*s)->getConvertedText(vmime::charsets::UTF_8);
    h.date = raw_header(raw, "Date").value_or("");
    h.created = raw_header(raw, "X-Mail-Created-Date").value_or("");
    h.uuid = raw_header(raw, "X-Universally-Unique-Identifier").value_or("");
    if (!h.date.empty()) {
        vmime::datetime d(h.date);
        if (d.getYear() > 1970) {
            std::tm tm{};
            tm.tm_year = d.getYear() - 1900, tm.tm_mon = d.getMonth() - 1, tm.tm_mday = d.getDay();
            tm.tm_hour = d.getHour(), tm.tm_min = d.getMinute(), tm.tm_sec = d.getSecond();
            h.date_value = timegm(&tm) - static_cast<std::time_t>(d.getZone()) * 60;
        }
    }
    auto ct = raw_header(raw, "Content-Type");
    h.multipart = ct && lower(*ct).starts_with("multipart/");
    return h;
}

std::optional<Attachment> find_attachment(std::string_view raw, std::string_view cid) {
    vmime::message msg;
    msg.parse(vmime::string(raw));
    std::function<std::optional<Attachment>(const vmime::bodyPart&)> walk = [&](const vmime::bodyPart& p) -> std::optional<Attachment> {
        const auto& h = *p.getHeader();
        if (h.hasField(vmime::fields::CONTENT_ID)) {
            std::string id = h.findField(vmime::fields::CONTENT_ID)->getValue()->generate();
            std::erase_if(id, [](char c) { return c == '<' || c == '>' || c == ' '; });
            if (id == cid) {
                std::ostringstream out;
                vmime::utility::outputStreamAdapter os(out);
                p.getBody()->getContents()->extract(os);  // transfer-decoded
                return Attachment{content_type(h), out.str()};
            }
        }
        for (const auto& sub : p.getBody()->getPartList())
            if (auto a = walk(*sub))
                return a;
        return std::nullopt;
    };
    return walk(msg);
}

std::string encode_quoted_printable(std::string_view s) {
    static const char* HEX = "0123456789ABCDEF";
    std::string out, line;
    auto emit = [&](const std::string& tok) {
        if (line.size() + tok.size() > 75) {  // leave room for the soft break's '='
            out += line + "=\r\n";
            line.clear();
        }
        line += tok;
    };
    size_t i = 0;
    while (i <= s.size()) {
        size_t eol = s.find('\n', i);
        bool last = eol == std::string_view::npos;
        std::string_view l = s.substr(i, last ? std::string_view::npos : eol - i);
        if (!l.empty() && l.back() == '\r')
            l.remove_suffix(1);
        for (size_t k = 0; k < l.size(); ++k) {
            unsigned char c = l[k];
            bool trailing_space = (c == ' ' || c == '\t') && k + 1 == l.size();
            if ((c >= 33 && c <= 126 && c != '=') || ((c == ' ' || c == '\t') && !trailing_space))
                emit(std::string(1, static_cast<char>(c)));
            else
                emit(std::string{'=', HEX[c >> 4], HEX[c & 15]});
        }
        out += line;
        line.clear();
        if (last)
            break;
        out += "\r\n";
        i = eol + 1;
    }
    return out;
}

std::string encode_subject(std::string_view utf8) {
    bool plain = utf8.size() <= 70 && std::all_of(utf8.begin(), utf8.end(), [](char c) { return c >= 32 && c < 127; }) &&
                 utf8.find("=?") == std::string_view::npos;
    if (plain)
        return std::string(utf8);
    // Encoded words of at most 45 bytes of text each, split between characters.
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        size_t n = std::min<size_t>(45, utf8.size() - i);
        while (n > 0 && i + n < utf8.size() && (static_cast<unsigned char>(utf8[i + n]) & 0xC0) == 0x80)
            --n;
        std::string_view chunk = utf8.substr(i, n);
        std::string enc;
        for (size_t k = 0; k < chunk.size(); k += 3) {
            unsigned v = static_cast<unsigned char>(chunk[k]) << 16;
            if (k + 1 < chunk.size()) v |= static_cast<unsigned char>(chunk[k + 1]) << 8;
            if (k + 2 < chunk.size()) v |= static_cast<unsigned char>(chunk[k + 2]);
            enc += B64[v >> 18 & 63];
            enc += B64[v >> 12 & 63];
            enc += k + 1 < chunk.size() ? B64[v >> 6 & 63] : '=';
            enc += k + 2 < chunk.size() ? B64[v & 63] : '=';
        }
        if (!out.empty())
            out += "\r\n ";
        out += "=?utf-8?B?" + enc + "?=";
        i += n;
    }
    return out;
}

std::string format_date(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %z", &tm);
    return buf;
}

}  // namespace jotmail
