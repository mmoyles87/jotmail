// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "convert.hpp"
#include "kitty.hpp"
#include "mime.hpp"
#include "clipboard.hpp"
#include "notes.hpp"
#include "theme.hpp"

namespace fs = std::filesystem;
using namespace jotmail;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<fs::path> multipart_fixtures() {
    std::vector<fs::path> out;
    for (const char* dir : {"synthetic", "private"}) {
        fs::path d = fs::path(JOTMAIL_FIXTURES) / dir;
        if (!fs::is_directory(d))
            continue;
        for (auto& e : fs::directory_iterator(d))
            if (e.path().extension() == ".eml" && note_headers(slurp(e.path())).multipart)
                out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> cids(const std::string& md) {
    std::vector<std::string> out;
    std::istringstream in(md);
    for (std::string line; std::getline(in, line);)
        if (line.starts_with("[attachment:") && line.ends_with("]"))
            out.push_back(line.substr(12, line.size() - 13));
    return out;
}

}  // namespace

TEST(BuildNote, ReadsBackAsTheSameNote) {
    std::string md = "# Café list\n- eggs\n- **milk** and *bread*\n\nSee https://example.com/a?b=1&c=2\n";
    std::string raw = build_note(md, "ABCDEF01-2345-4678-89AB-CDEF01234567", "Mon, 1 Jan 2024 10:00:00 -0500",
                                 "me@example.com", 1760000000);
    Note n = parse_note({"7", raw});
    EXPECT_EQ(n.uuid, "ABCDEF01-2345-4678-89AB-CDEF01234567");
    EXPECT_EQ(n.title, "Café list");
    EXPECT_EQ(n.created, "Mon, 1 Jan 2024 10:00:00 -0500");
    EXPECT_EQ(n.date, 1760000000);
    EXPECT_FALSE(n.attachments);
    EXPECT_EQ(n.md, html_to_md(md_to_html(md)));
    EXPECT_NE(raw.find("X-Uniform-Type-Identifier: com.apple.mail-note\r\n"), std::string::npos);
    std::istringstream lines(raw);
    for (std::string l; std::getline(lines, l);)
        EXPECT_LE(l.size(), 77u) << l;  // 76 plus the CR
}

TEST(BuildNote, LongNonAsciiTitleIsEncodedAndFolded) {
    std::string title(60, 'x');
    title += "ünïcödé and more words to make it long enough to need several encoded words";
    std::string raw = build_note("# " + title + "\n", new_uuid(), "x", "me@example.com", 0);
    EXPECT_EQ(parse_note({"1", raw}).title, title_of("# " + title));
}

TEST(RebuildNote, KeepsImagesByteForByteAndReplacesTheText) {
    auto files = multipart_fixtures();
    ASSERT_FALSE(files.empty());
    for (auto& f : files) {
        SCOPED_TRACE(f.string());
        std::string raw = slurp(f);
        Note n = parse_note({"1", raw});
        std::string md = n.md + "an added line\n";
        std::string out = rebuild_note(raw, md, "example.com", 1760000000);
        Note back = parse_note({"2", out});
        EXPECT_EQ(back.md, html_to_md(md_to_html(md)));
        EXPECT_EQ(back.uuid, n.uuid);
        EXPECT_EQ(back.created, n.created);
        EXPECT_EQ(back.date, 1760000000);
        for (auto& cid : cids(n.md)) {
            auto before = find_attachment(raw, cid), after = find_attachment(out, cid);
            ASSERT_TRUE(before && after) << cid;
            EXPECT_EQ(before->bytes, after->bytes);
            EXPECT_EQ(before->type, after->type);
        }
    }
}

TEST(RebuildNote, DropsImagesWhoseLineIsDeleted) {
    std::string raw = slurp(fs::path(JOTMAIL_FIXTURES) / "synthetic/image.eml");
    Note n = parse_note({"1", raw});
    auto ids = cids(n.md);
    ASSERT_EQ(ids.size(), 1u);
    std::string md = n.md;
    md.erase(md.find("[attachment:"), md.find('\n', md.find("[attachment:")) - md.find("[attachment:") + 1);
    std::string out = rebuild_note(raw, md, "example.com", 0);
    EXPECT_FALSE(find_attachment(out, ids[0]));
    EXPECT_EQ(parse_note({"2", out}).md, html_to_md(md_to_html(md)));
}

TEST(QuotedPrintable, RoundTripsAndKeepsLinesShort) {
    std::mt19937 rng(7);
    for (int i = 0; i < 500; ++i) {
        std::string s;
        int n = rng() % 300;
        for (int k = 0; k < n; ++k) {
            int r = rng() % 10;
            s += r < 6 ? static_cast<char>('a' + rng() % 26) : r == 6 ? ' ' : r == 7 ? '\n' : r == 8 ? '=' : static_cast<char>(rng() % 256);
        }
        std::string enc = encode_quoted_printable(s);
        std::string want = s;
        for (size_t p = 0; (p = want.find('\n', p)) != std::string::npos; p += 2)
            want.replace(p, 1, "\r\n");
        // a CR the input already had before its LF is not doubled
        std::string dec = decode_quoted_printable(enc);
        std::string norm;
        for (size_t k = 0; k < want.size(); ++k)
            if (!(want[k] == '\r' && k + 1 < want.size() && want[k + 1] == '\r'))
                norm += want[k];
        EXPECT_EQ(dec, norm) << "case " << i;
        std::istringstream lines(enc);
        for (std::string l; std::getline(lines, l);)
            EXPECT_LE(l.size(), 77u);
    }
}

TEST(Subject, PlainWhenAsciiEncodedOtherwise) {
    EXPECT_EQ(encode_subject("Groceries"), "Groceries");
    std::string enc = encode_subject("Café 😀");
    EXPECT_TRUE(enc.starts_with("=?utf-8?B?"));
    EXPECT_EQ(note_headers("Subject: " + enc + "\r\n\r\nx").subject, "Café 😀");
}

TEST(ParseNotes, KeepsTheNewestVersionOfEachNoteNewestFirst) {
    auto mk = [](const char* uid, const char* uuid, const char* date, const char* text) {
        std::string raw = std::string("Date: ") + date + "\r\nSubject: " + text + "\r\nX-Universally-Unique-Identifier: " +
                          uuid + "\r\nContent-Type: text/html\r\n\r\n<div>" + text + "</div>";
        return RawNote{uid, raw};
    };
    auto notes = parse_notes({mk("1", "A", "Mon, 1 Jan 2024 10:00:00 +0000", "old a"),
                              mk("2", "B", "Tue, 2 Jan 2024 10:00:00 +0000", "b"),
                              mk("3", "A", "Mon, 1 Jan 2024 09:00:00 +0000", "new a")});
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].uuid, "B");
    EXPECT_EQ(notes[1].md, "new a\n");
    EXPECT_EQ(notes[1].uid, 3u);
}

TEST(Kitty, PlaceholderRowEncodesRowAndColumn) {
    // U+10EEEE, the row's diacritic, then the column's: row 1 is U+030D, columns 0 and 1 are U+0305, U+030D
    EXPECT_EQ(kitty::placeholder_row(1, 2), "\xF4\x8E\xBB\xAE\xCC\x8D\xCC\x85\xF4\x8E\xBB\xAE\xCC\x8D\xCC\x8D");
}

// --- adding images -----------------------------------------------------------------

namespace {

// The 1x1 PNG from the image fixture, written to a temporary file.
fs::path temp_png() {
    auto raw = slurp(fs::path(JOTMAIL_FIXTURES) / "synthetic/image.eml");
    auto img = find_attachment(raw, "0BDCA9D6-A671-43C3-AABC-733FDC4BADE8");
    fs::path p = fs::temp_directory_path() / ("jotmail-test-" + new_uuid() + ".png");
    std::ofstream(p, std::ios::binary) << img->bytes;
    return p;
}

}  // namespace

TEST(AttachFiles, TurnsPathLinesIntoNewImages) {
    fs::path png = temp_png();
    std::string md = "# Shed\n[attachment:KEEP-ME]\n[attachment:" + png.string() + "]\n[attachment:not-a-path]\ntext\n";
    auto [out, images] = attach_files(md, {"KEEP-ME"});
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].type, "image/png");
    EXPECT_EQ(images[0].filename, png.filename().string());
    EXPECT_EQ(images[0].bytes, slurp(png));
    EXPECT_EQ(out, "# Shed\n[attachment:KEEP-ME]\n[attachment:" + images[0].cid + "]\n[attachment:not-a-path]\ntext\n");
    fs::remove(png);
}

TEST(AttachFiles, RefusesMissingFilesAndNonImages) {
    EXPECT_THROW(attach_files("[attachment:/no/such/file.png]\n", {}), std::runtime_error);
    fs::path txt = fs::temp_directory_path() / ("jotmail-test-" + new_uuid() + ".txt");
    std::ofstream(txt) << "not an image";
    EXPECT_THROW(attach_files("[attachment:" + txt.string() + "]\n", {}), std::runtime_error);
    fs::remove(txt);
}

TEST(AttachFiles, ImageInANewNoteMakesItMultipart) {
    fs::path png = temp_png();
    auto [md, images] = attach_files("# Photo\n[attachment:" + png.string() + "]\n", {});
    std::string raw = build_note(md, new_uuid(), "x", "me@example.com", 0, images);
    Note n = parse_note({"1", raw});
    EXPECT_TRUE(n.attachments);
    EXPECT_EQ(n.md, "# Photo\n[attachment:" + images[0].cid + "]\n");
    auto back = find_attachment(raw, images[0].cid);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->bytes, images[0].bytes);
    EXPECT_EQ(back->type, "image/png");
    fs::remove(png);
}

TEST(AttachFiles, ImageAddedToANoteThatHasOne) {
    std::string raw = slurp(fs::path(JOTMAIL_FIXTURES) / "synthetic/image.eml");
    Note n = parse_note({"1", raw});
    fs::path png = temp_png();
    auto [md, images] = attach_files(n.md + "[attachment:" + png.string() + "]\n", attachment_ids(n.md));
    ASSERT_EQ(images.size(), 1u);
    std::string out = rebuild_note(raw, md, "example.com", 0, images);
    EXPECT_TRUE(find_attachment(out, "0BDCA9D6-A671-43C3-AABC-733FDC4BADE8"));
    EXPECT_TRUE(find_attachment(out, images[0].cid));
    EXPECT_EQ(parse_note({"2", out}).md, html_to_md(md_to_html(md)));
    fs::remove(png);
}

TEST(Clipboard, ReadsAnImageFromPasteCommand) {
    fs::path png = temp_png();
    auto img = clipboard_image("cat '" + png.string() + "'");
    EXPECT_EQ(img.type, "image/png");
    EXPECT_EQ(img.bytes, slurp(png));
    EXPECT_THROW(clipboard_image("echo hello"), std::runtime_error);
    EXPECT_THROW(clipboard_image("false"), std::runtime_error);
    fs::remove(png);
}

TEST(Theme, PresetsParseAndColoursOverride) {
    for (auto& p : theme_presets())
        EXPECT_NO_THROW(make_theme(p, {})) << p;
    Theme t = make_theme("forest", {{"accent", "#fabd2f"}, {"panel", "default"}, {"muted", "bright-black"}, {"link", "214"}});
    EXPECT_EQ(t.accent, ftxui::Color::RGB(0xfa, 0xbd, 0x2f));
    EXPECT_EQ(t.panel, ftxui::Color(ftxui::Color::Default));
    EXPECT_EQ(t.muted, ftxui::Color(ftxui::Color::GrayDark));
    EXPECT_EQ(t.link, ftxui::Color(ftxui::Color::Palette256(214)));
    EXPECT_THROW(make_theme("nope", {}), std::runtime_error);
    EXPECT_THROW(make_theme("forest", {{"acent", "#ffffff"}}), std::runtime_error);
    EXPECT_THROW(make_theme("forest", {{"accent", "#ffff"}}), std::runtime_error);
    EXPECT_THROW(make_theme("forest", {{"accent", "300"}}), std::runtime_error);
}
