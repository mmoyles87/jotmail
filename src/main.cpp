// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <strings.h>
#include <unistd.h>

#include "config.hpp"
#include "editor.hpp"
#include "notes.hpp"
#include "store.hpp"
#include "tui.hpp"

namespace {

const char* USAGE = R"(jotmail: notes from your IMAP mailbox, in a terminal

usage:
  jotmail [options]                    browse and edit notes
  jotmail [options] list               number, date and title of every note, newest first
  jotmail [options] show NOTE          print a note's text
  jotmail [options] new [FILE]         add a note from FILE, or standard input
  jotmail [options] edit NOTE [FILE]   replace a note's text from FILE, or edit it in $EDITOR
  jotmail [options] rm NOTE            move a note to Trash
  jotmail [options] dump DIR           save every message as DIR/<uid>.eml, byte for byte

NOTE is a number from `list` or the start of a note's UUID.

options:
  -a, --account NAME   an account from the config (default: the config's `default`)
  -f, --folder NAME    another folder than the account's (default: Notes)
  -h, --help
  -V, --version

Accounts are read from ~/.config/jotmail/config.toml.
)";

std::string read_input(const std::string& file) {
    std::ostringstream s;
    if (file.empty() || file == "-") {
        s << std::cin.rdbuf();
    } else {
        std::ifstream f(file, std::ios::binary);
        if (!f)
            throw std::runtime_error("cannot read " + file);
        s << f.rdbuf();
    }
    return s.str();
}

const jotmail::Note& find(const std::vector<jotmail::Note>& notes, const std::string& ref) {
    // A number in range is a position in `list`; anything else, digits included, a UUID prefix.
    if (!ref.empty() && ref.size() <= 6 && ref.find_first_not_of("0123456789") == std::string::npos) {
        size_t i = std::stoul(ref);
        if (i >= 1 && i <= notes.size())
            return notes[i - 1];
    }
    const jotmail::Note* hit = nullptr;
    for (auto& n : notes)
        if (n.uuid.size() >= ref.size() && strncasecmp(n.uuid.c_str(), ref.c_str(), ref.size()) == 0) {
            if (hit)
                throw std::runtime_error(ref + " matches more than one note");
            hit = &n;
        }
    if (!hit)
        throw std::runtime_error("no note matches " + ref);
    return *hit;
}

std::string when(std::time_t t) {
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string account, folder;
    while (!args.empty() && args[0].starts_with("-") && args[0] != "-") {
        if ((args[0] == "-a" || args[0] == "--account") && args.size() >= 2) {
            account = args[1];
        } else if ((args[0] == "-f" || args[0] == "--folder") && args.size() >= 2) {
            folder = args[1];
        } else if (args[0] == "-h" || args[0] == "--help") {
            std::fputs(USAGE, stdout);
            return 0;
        } else if (args[0] == "-V" || args[0] == "--version") {
            std::puts("jotmail " JOTMAIL_VERSION);
            return 0;
        } else {
            std::fputs(USAGE, stderr);
            return 2;
        }
        args.erase(args.begin(), args.begin() + 2);
    }

    try {
        auto config = jotmail::Config::load(jotmail::Config::default_path());
        jotmail::Account acc = config.account(account);
        if (!folder.empty())
            acc.folder = folder;

        if (args.empty())
            return jotmail::run_tui(acc, config);

        jotmail::Store store(acc);
        jotmail::Notes notes(store);
        const std::string& cmd = args[0];

        if (cmd == "list" && args.size() == 1) {
            auto all = notes.load();
            for (size_t i = 0; i < all.size(); ++i)
                std::printf("%3zu  %s  %s%s\n", i + 1, when(all[i].date).c_str(), all[i].title.c_str(),
                            all[i].attachments ? "  [images]" : "");
            return 0;
        }
        if (cmd == "show" && args.size() == 2) {
            std::fputs(find(notes.load(), args[1]).md.c_str(), stdout);
            return 0;
        }
        if (cmd == "new" && args.size() <= 2) {
            std::string md = read_input(args.size() == 2 ? args[1] : "");
            if (md.find_first_not_of(" \t\r\n#") == std::string::npos)
                throw std::runtime_error("the note is empty");
            notes.save(nullptr, md);
            return 0;
        }
        if (cmd == "edit" && (args.size() == 2 || args.size() == 3)) {
            auto all = notes.load();
            const jotmail::Note& n = find(all, args[1]);
            std::string md;
            if (args.size() == 3 || !isatty(STDIN_FILENO)) {
                md = read_input(args.size() == 3 ? args[2] : "");
            } else {
                auto edited = jotmail::edit_text(n.md);
                if (!edited)
                    return 0;
                md = *edited;
            }
            try {
                notes.save(&n, md);
            } catch (const jotmail::BothKept& e) {
                std::fprintf(stderr, "jotmail: saved, but %s\n", e.what());
            } catch (const jotmail::Conflict&) {
                auto kept = jotmail::keep_unsaved(md);
                throw std::runtime_error("the note changed on the server while you edited; not saved, your text is in " + kept.string());
            } catch (const std::exception& e) {  // the editor's file is gone by now: keep the text
                auto kept = jotmail::keep_unsaved(md);
                throw std::runtime_error(std::string(e.what()) + "; not saved, your text is in " + kept.string());
            }
            return 0;
        }
        if (cmd == "rm" && args.size() == 2) {
            notes.remove(find(notes.load(), args[1]));
            return 0;
        }
        if (cmd == "dump" && args.size() == 2) {
            std::filesystem::path dir(args[1]);
            std::filesystem::create_directories(dir);
            auto raw = store.fetch_all();
            for (auto& n : raw)
                std::ofstream(dir / (n.uid + ".eml"), std::ios::binary) << n.bytes;
            std::printf("saved %zu messages to %s\n", raw.size(), dir.c_str());
            return 0;
        }
        std::fputs(USAGE, stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "jotmail: %s\n", e.what());
        return 1;
    }
}
