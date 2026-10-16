// SPDX-License-Identifier: GPL-3.0-or-later
// Every X.eml under the fixture directories has the reference implementation's output
// beside it: X.body.html, X.md, X.out.html and X.title.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "convert.hpp"
#include "mime.hpp"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<fs::path> fixtures() {
    std::vector<fs::path> out;
    for (const char* dir : {"synthetic", "private"}) {
        fs::path d = fs::path(JOTMAIL_FIXTURES) / dir;
        if (!fs::is_directory(d))
            continue;
        for (auto& e : fs::directory_iterator(d))
            if (e.path().extension() == ".eml" && fs::exists(fs::path(e.path()).replace_extension(".md")))
                out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

class Fixture : public testing::TestWithParam<fs::path> {
protected:
    fs::path with(const char* ext) const { return fs::path(GetParam()).replace_extension(ext); }
};

TEST_P(Fixture, DecodesTheHtmlPart) {
    EXPECT_EQ(jotmail::note_html(slurp(GetParam())), slurp(with(".body.html")));
}

TEST_P(Fixture, HtmlToMarkdown) {
    EXPECT_EQ(jotmail::html_to_md(slurp(with(".body.html"))), slurp(with(".md")));
}

TEST_P(Fixture, MarkdownToHtml) {
    EXPECT_EQ(jotmail::md_to_html(slurp(with(".md"))), slurp(with(".out.html")));
}

TEST_P(Fixture, Title) {
    EXPECT_EQ(jotmail::title_of(slurp(with(".md"))), slurp(with(".title")));
}

INSTANTIATE_TEST_SUITE_P(Notes, Fixture, testing::ValuesIn(fixtures()), [](const auto& info) {
    std::string name = info.param.parent_path().filename().string() + "_" + info.param.stem().string();
    for (auto& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    return name;
});

}  // namespace

// --- differential fuzz: random inputs with the reference implementation's outputs ------

namespace {

std::vector<std::vector<std::string>> rows(const char* file) {
    std::vector<std::vector<std::string>> out;
    std::istringstream in(slurp(fs::path(JOTMAIL_FIXTURES) / file));
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> cols;
        size_t a = 0;
        for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1)
            cols.push_back(jotmail::decode_base64(line.substr(a, b - a)));
        cols.push_back(jotmail::decode_base64(line.substr(a)));
        out.push_back(std::move(cols));
    }
    return out;
}

std::string show(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

void check(const char* what, const std::string& in, const std::string& got, const std::string& want, int& failures) {
    if (got == want)
        return;
    if (++failures <= 8)
        ADD_FAILURE() << what << "\n  input: " << show(in) << "\n  got:   " << show(got) << "\n  want:  " << show(want);
}

}  // namespace

TEST(Fuzz, HtmlToMarkdown) {
    int failures = 0;
    auto all = rows("fuzz-html.tsv");
    ASSERT_GT(all.size(), 1000u);
    for (auto& r : all)
        check("html_to_md", r[0], jotmail::html_to_md(r[0]), r[1], failures);
    EXPECT_EQ(failures, 0) << "of " << all.size();
}

TEST(Fuzz, MarkdownToHtmlAndTitle) {
    int failures = 0;
    auto all = rows("fuzz-md.tsv");
    ASSERT_GT(all.size(), 1000u);
    for (auto& r : all) {
        check("md_to_html", r[0], jotmail::md_to_html(r[0]), r[1], failures);
        check("title_of", r[0], jotmail::title_of(r[0]), r[2], failures);
    }
    EXPECT_EQ(failures, 0) << "of " << all.size();
}
