// SPDX-License-Identifier: GPL-3.0-or-later
#include "notes.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string_view>

#include "convert.hpp"
#include "mime.hpp"

namespace jotmail {

namespace {

constexpr const char* UUID_HDR = "X-Universally-Unique-Identifier";

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string domain_of(const std::string& user) {
    auto at = user.find('@');
    return at == std::string::npos ? user : user.substr(at + 1);
}

// A header block split into fields, each field with its continuation lines and CRLF.
std::vector<std::string> split_fields(std::string_view head) {
    std::vector<std::string> fields;
    size_t pos = 0;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        eol = eol == std::string_view::npos ? head.size() : eol + 2;
        std::string_view line = head.substr(pos, eol - pos);
        if ((line[0] == ' ' || line[0] == '\t') && !fields.empty())
            fields.back() += line;
        else
            fields.emplace_back(line);
        pos = eol;
    }
    return fields;
}

std::string field_name(const std::string& field) {
    return lower(field.substr(0, field.find(':')));
}

std::string field_value(const std::string& field) {
    size_t colon = field.find(':');
    if (colon == std::string::npos)
        return {};
    std::string v = field.substr(colon + 1);
    std::erase(v, '\r');
    std::replace(v.begin(), v.end(), '\n', ' ');
    size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
}

// The boundary parameter of a Content-Type value.
std::optional<std::string> boundary_of(const std::string& content_type) {
    std::string lc = lower(content_type);
    size_t p = lc.find("boundary=");
    if (p == std::string::npos)
        return std::nullopt;
    p += 9;
    if (p < content_type.size() && content_type[p] == '"') {
        size_t e = content_type.find('"', p + 1);
        return content_type.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    size_t e = content_type.find_first_of("; \t", p);
    return content_type.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

std::string base64_lines(const std::string& bytes) {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out, line;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 2]);
        line += B64[v >> 18 & 63];
        line += B64[v >> 12 & 63];
        line += i + 1 < bytes.size() ? B64[v >> 6 & 63] : '=';
        line += i + 2 < bytes.size() ? B64[v & 63] : '=';
        if (line.size() == 76) {
            out += line + "\r\n";
            line.clear();
        }
    }
    return out + line;
}

// An image part as Apple Notes writes one, without the delimiter line.
std::string image_part(const NewImage& img) {
    return "Content-Transfer-Encoding: base64\r\nContent-Disposition: inline;\r\n\tfilename=\"" + img.filename +
           "\"\r\nContent-Type: " + img.type + ";\r\n\tname=\"" + img.filename + "\"\r\nContent-Id: <" + img.cid +
           ">\r\n\r\n" + base64_lines(img.bytes);
}

std::string html_part(const std::string& md) {
    return "Content-Transfer-Encoding: quoted-printable\r\nContent-Type: text/html;\r\n\tcharset=utf-8\r\n\r\n" +
           encode_quoted_printable(md_to_html(md));
}

// The image type from the file's first bytes.
std::optional<std::string> image_type(const std::string& b) {
    if (b.starts_with("\x89PNG\r\n\x1a\n")) return "image/png";
    if (b.starts_with("\xFF\xD8\xFF")) return "image/jpeg";
    if (b.starts_with("GIF87a") || b.starts_with("GIF89a")) return "image/gif";
    if (b.size() > 12 && b.starts_with("RIFF") && b.compare(8, 4, "WEBP") == 0) return "image/webp";
    if (b.size() > 12 && b.compare(4, 4, "ftyp") == 0) {
        std::string brand = b.substr(8, 4);
        if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1" || brand == "hevc") return "image/heic";
    }
    return std::nullopt;
}

// A filename safe to put in a quoted header parameter: printable ASCII, no quotes or backslashes.
std::string safe_filename(const std::string& name) {
    std::string out;
    for (unsigned char c : name)
        out += (c >= 32 && c < 127 && c != '"' && c != '\\') ? static_cast<char>(c) : '_';
    return out.empty() ? "image" : out;
}

}  // namespace

std::vector<std::string> attachment_ids(const std::string& md) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a < md.size()) {
        size_t e = md.find('\n', a);
        std::string line = md.substr(a, e == std::string::npos ? std::string::npos : e - a);
        if (line.starts_with("[attachment:")) {
            size_t close = line.rfind(']');
            if (close != std::string::npos && close > 12 && line.find_first_not_of(" \t\r", close + 1) == std::string::npos)
                out.push_back(line.substr(12, close - 12));
        }
        if (e == std::string::npos)
            break;
        a = e + 1;
    }
    return out;
}

std::pair<std::string, std::vector<NewImage>> attach_files(const std::string& md, const std::vector<std::string>& existing) {
    std::string out;
    std::vector<NewImage> images;
    size_t a = 0;
    while (a <= md.size()) {
        size_t e = md.find('\n', a);
        std::string line = md.substr(a, e == std::string::npos ? std::string::npos : e - a);
        auto ids = attachment_ids(line);
        if (ids.size() == 1 && std::find(existing.begin(), existing.end(), ids[0]) == existing.end()) {
            std::string target = ids[0];
            bool is_path = target.starts_with("/") || target.starts_with("~") || target.starts_with("./") ||
                           target.starts_with("../") || target.find('/') != std::string::npos;
            if (is_path) {
                std::string path = target;
                if (path.starts_with("~/") || path == "~")
                    if (const char* home = std::getenv("HOME"))
                        path = std::string(home) + path.substr(1);
                std::ifstream f(path, std::ios::binary);
                if (!f)
                    throw std::runtime_error("no image at " + target);
                std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                auto type = image_type(bytes);
                if (!type)
                    throw std::runtime_error(target + " is not a PNG, JPEG, GIF, WebP or HEIC image");
                NewImage img{new_uuid(), *type, safe_filename(std::filesystem::path(path).filename().string()), std::move(bytes)};
                line = "[attachment:" + img.cid + "]";
                images.push_back(std::move(img));
            }
        }
        out += line;
        if (e == std::string::npos)
            break;
        out += "\n";
        a = e + 1;
    }
    return {out, images};
}

std::string new_uuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    unsigned char b[16];
    for (int i = 0; i < 16; i += 8) {
        uint64_t r = rng();
        for (int k = 0; k < 8; ++k)
            b[i + k] = static_cast<unsigned char>(r >> (8 * k));
    }
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    static const char* HEX = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            s += '-';
        s += HEX[b[i] >> 4];
        s += HEX[b[i] & 15];
    }
    return s;
}

Note parse_note(const RawNote& r) {
    NoteHeaders h = note_headers(r.bytes);
    Note n;
    n.uid = std::stoul(r.uid);
    n.uuid = h.uuid.empty() ? "no-uuid-" + r.uid : h.uuid;
    n.title = h.subject.empty() ? "Untitled" : h.subject;
    n.date = h.date_value.value_or(std::time(nullptr));
    n.created = !h.created.empty() ? h.created : !h.date.empty() ? h.date : format_date(std::time(nullptr));
    n.md = html_to_md(note_html(r.bytes));
    n.attachments = h.multipart;
    if (h.multipart)
        n.raw = r.bytes;
    return n;
}

std::vector<Note> parse_notes(const std::vector<RawNote>& raw) {
    std::vector<Note> out;  // in the order each UUID was first seen, as a Python dict keeps it
    std::map<std::string, size_t> index;
    for (auto& r : raw) {
        Note n = parse_note(r);
        auto it = index.find(n.uuid);
        if (it == index.end()) {
            index[n.uuid] = out.size();
            out.push_back(std::move(n));
        } else if (n.uid > out[it->second].uid) {
            out[it->second] = std::move(n);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.date > b.date; });
    return out;
}

std::string build_note(const std::string& md, const std::string& uuid, const std::string& created,
                       const std::string& from, std::time_t now, const std::vector<NewImage>& images) {
    std::string boundary = "Apple-Mail=_" + new_uuid();
    std::string h;
    h += "From: " + from + "\r\n";
    h += "X-Uniform-Type-Identifier: com.apple.mail-note\r\n";
    if (images.empty()) {
        h += "Content-Type: text/html;\r\n\tcharset=utf-8\r\n";
        h += "Content-Transfer-Encoding: quoted-printable\r\n";
    } else {
        h += "Content-Type: multipart/related;\r\n\ttype=\"text/html\";\r\n\tboundary=\"" + boundary + "\"\r\n";
    }
    h += "Mime-Version: 1.0 (jotmail)\r\n";
    h += "Date: " + format_date(now) + "\r\n";
    h += "X-Mail-Created-Date: " + created + "\r\n";
    h += "Subject: " + encode_subject(title_of(md)) + "\r\n";
    h += std::string(UUID_HDR) + ": " + uuid + "\r\n";
    h += "Message-Id: <" + new_uuid() + "@" + domain_of(from) + ">\r\n";
    if (images.empty())
        return h + "\r\n" + encode_quoted_printable(md_to_html(md));
    std::string body = "--" + boundary + "\r\n" + html_part(md) + "\r\n";
    for (auto& img : images)
        body += "--" + boundary + "\r\n" + image_part(img) + "\r\n";
    return h + "\r\n" + body + "--" + boundary + "--\r\n";
}

std::string rebuild_note(const std::string& raw, const std::string& md, const std::string& domain, std::time_t now,
                         const std::vector<NewImage>& images) {
    size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos)
        throw std::runtime_error("the note has no header/body separator");
    std::vector<std::string> fields = split_fields(std::string_view(raw).substr(0, head_end + 2));
    std::string body = raw.substr(head_end + 4);

    std::optional<std::string> boundary;
    for (auto& f : fields)
        if (field_name(f) == "content-type")
            boundary = boundary_of(field_value(f));
    if (!boundary)
        throw std::runtime_error("the note is multipart but has no boundary");
    const std::string delim = "--" + *boundary;

    // Delimiter lines: "--boundary" at the start of a line.
    std::vector<size_t> at;
    for (size_t p = body.find(delim); p != std::string::npos; p = body.find(delim, p + 1))
        if (p == 0 || body.compare(p - 2, 2, "\r\n") == 0)
            at.push_back(p);
    if (at.size() < 2)
        throw std::runtime_error("the note's MIME parts could not be found");

    std::set<std::string> kept;
    for (size_t a = 0; a < md.size();) {
        size_t e = md.find('\n', a);
        std::string line = md.substr(a, e == std::string::npos ? std::string::npos : e - a);
        if (line.starts_with("[attachment:")) {
            size_t close = line.find(']');
            std::string cid = close == std::string::npos ? "" : line.substr(12, close - 12);
            std::string rest = close == std::string::npos ? "x" : line.substr(close + 1);
            bool blank = rest.find_first_not_of(" \t\r") == std::string::npos;
            if (!cid.empty() && cid.find_first_of(" \t") == std::string::npos && blank)
                kept.insert(cid);
        }
        if (e == std::string::npos)
            break;
        a = e + 1;
    }

    std::string out_body = body.substr(0, at[0]);  // the preamble
    for (size_t i = 0; i + 1 < at.size(); ++i) {
        size_t start = body.find("\r\n", at[i]);
        if (start == std::string::npos || start > at[i + 1])
            continue;
        start += 2;
        size_t end = at[i + 1] >= 2 ? at[i + 1] - 2 : at[i + 1];  // the CRLF before a delimiter belongs to it
        std::string part = body.substr(start, end > start ? end - start : 0);

        size_t ph_end = part.find("\r\n\r\n");
        std::string_view ph = std::string_view(part).substr(0, ph_end == std::string::npos ? part.size() : ph_end + 2);
        std::string type, cid;
        std::vector<std::string> other;
        for (auto& f : split_fields(ph)) {
            std::string name = field_name(f);
            if (name == "content-type") {
                std::string v = lower(field_value(f));
                type = v.substr(0, v.find(';'));
                type.erase(type.find_last_not_of(" \t") + 1);
            } else if (name == "content-id") {
                cid = field_value(f);
                std::erase_if(cid, [](char c) { return c == '<' || c == '>' || c == ' '; });
            }
            if (!name.starts_with("content-"))
                other.push_back(f);
        }
        if (type.empty())
            type = "text/plain";

        if (type == "text/html" && cid.empty()) {
            std::string fresh;
            for (auto& f : other)
                fresh += f;
            fresh += html_part(md);
            out_body += delim + "\r\n" + fresh + "\r\n";
        } else if (!cid.empty() && kept.count(cid)) {
            out_body += delim + "\r\n" + part + "\r\n";
        }
    }
    for (auto& img : images)
        out_body += delim + "\r\n" + image_part(img) + "\r\n";
    out_body += body.substr(at.back());  // the closing delimiter and epilogue

    std::string head;
    for (auto& f : fields) {
        std::string name = field_name(f);
        if (name != "date" && name != "subject" && name != "message-id")
            head += f;
    }
    head += "Date: " + format_date(now) + "\r\n";
    head += "Subject: " + encode_subject(title_of(md)) + "\r\n";
    head += "Message-Id: <" + new_uuid() + "@" + domain + ">\r\n";
    return head + "\r\n" + out_body;
}

std::vector<Note> Notes::load() {
    return parse_notes(store_.fetch_all());
}

void Notes::save(const Note* note, const std::string& md) {
    if (note) {
        auto found = store_.search_header(UUID_HDR, note->uuid);
        if (!found.empty() && *std::max_element(found.begin(), found.end()) != note->uid)
            throw Conflict(note->title);
    }
    auto [text, images] = attach_files(md, note ? attachment_ids(note->md) : std::vector<std::string>{});
    const std::string& user = store_.account().user;
    std::time_t now = std::time(nullptr);
    std::string raw;
    if (note && !note->raw.empty()) {
        raw = rebuild_note(note->raw, text, domain_of(user), now, images);
    } else {
        std::string uuid = note && !note->uuid.starts_with("no-uuid-") ? note->uuid : new_uuid();
        std::string created = note ? note->created : format_date(now);
        raw = build_note(text, uuid, created, user, now, images);
    }
    store_.append(raw);
    if (note)
        store_.remove({note->uid});
}

void Notes::remove(const Note& note) {
    // every copy of the note, so an older one does not take its place
    std::vector<unsigned long> uids{note.uid};
    if (!note.uuid.starts_with("no-uuid-"))
        for (auto u : store_.search_header(UUID_HDR, note.uuid))
            if (u != note.uid)
                uids.push_back(u);
    store_.trash(uids);
}

}  // namespace jotmail
