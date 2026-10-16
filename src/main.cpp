// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "config.hpp"
#include "store.hpp"

namespace {

const char* USAGE = R"(jotmail: the iPhone's IMAP notes in a terminal

usage:
  jotmail [-a ACCOUNT] list        print each note's UID and subject
  jotmail [-a ACCOUNT] dump DIR    save every note as DIR/<uid>.eml, byte for byte

Accounts are read from ~/.config/jotmail/config.toml.
)";

std::string header(const std::string& raw, const std::string& name) {
    std::regex re("(^|\r?\n)" + name + ":[ \t]*([^\r\n]*)", std::regex::icase);
    std::smatch m;
    return std::regex_search(raw, m, re) ? m[2].str() : std::string{};
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string account;
    if (args.size() >= 2 && (args[0] == "-a" || args[0] == "--account")) {
        account = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        std::fputs(USAGE, args.empty() ? stderr : stdout);
        return args.empty() ? 2 : 0;
    }

    try {
        auto config = jotmail::Config::load(jotmail::Config::default_path());
        jotmail::Store store(config.account(account));

        if (args[0] == "list") {
            for (auto& n : store.fetch_all())
                std::printf("%6s  %s\n", n.uid.c_str(), header(n.bytes, "Subject").c_str());
            return 0;
        }
        if (args[0] == "dump" && args.size() == 2) {
            std::filesystem::path dir(args[1]);
            std::filesystem::create_directories(dir);
            auto notes = store.fetch_all();
            for (auto& n : notes)
                std::ofstream(dir / (n.uid + ".eml"), std::ios::binary) << n.bytes;
            std::printf("saved %zu notes to %s\n", notes.size(), dir.c_str());
            return 0;
        }
        std::fputs(USAGE, stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "jotmail: %s\n", e.what());
        return 1;
    }
}
