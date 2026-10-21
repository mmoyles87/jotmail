// SPDX-License-Identifier: GPL-3.0-or-later
#include "tui.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "convert.hpp"
#include "editor.hpp"
#include "kitty.hpp"
#include "mime.hpp"
#include "notes.hpp"
#include "clipboard.hpp"
#include "text.hpp"
#include "theme.hpp"

namespace jotmail {

namespace {

using namespace ftxui;

Theme T;  // set by run_tui

constexpr int LIST_WIDTH = 38;

std::string when(std::time_t t) {
    std::time_t now = std::time(nullptr);
    std::tm d{}, n{};
    localtime_r(&t, &d);
    localtime_r(&now, &n);
    char buf[32];
    const char* fmt = (d.tm_year == n.tm_year && d.tm_yday == n.tm_yday) ? "%H:%M"
                      : (now - t < 7 * 86400 && now >= t)                  ? "%a %H:%M"
                      : d.tm_year != n.tm_year                             ? "%b %d %Y"
                                                                           : "%b %d";
    std::strftime(buf, sizeof buf, fmt, &d);
    return buf;
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void pop_utf8(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

// At most `cols` display columns of s, with an ellipsis if it was cut.
std::string fit(const std::string& s, int cols) {
    if (string_width(s) <= cols)
        return s;
    std::string out;
    for (auto& g : Utf8ToGlyphs(s)) {
        if (string_width(out + g) > cols - 1)
            break;
        out += g;
    }
    return out + "…";
}

// s cut into pieces at most `cols` columns wide
std::vector<std::string> chop(const std::string& s, int cols) {
    std::vector<std::string> out(1);
    for (auto& g : Utf8ToGlyphs(s)) {
        if (string_width(out.back() + g) > cols)
            out.emplace_back();
        out.back() += g;
    }
    return out;
}

// A line with its inline formatting, word-wrapped to `width` columns. The wrapping is done here
// rather than with hflow, whose height inside a vbox is not reliable.
// Where each link is on screen after the frame is drawn, for clicks.
using LinkBoxes = std::deque<std::pair<Box, std::string>>;

Element styled(const std::string& line, Color base, int width, LinkBoxes& links) {
    width = std::max(width, 1);
    Elements rows, row;
    int used = 0;
    for (auto& span : inline_spans(line)) {
        size_t a = 0;
        while (a < span.text.size()) {
            size_t b = span.text.find(' ', a);
            b = b == std::string::npos ? span.text.size() : b + 1;
            for (auto& piece : chop(span.text.substr(a, b - a), width)) {
                std::string word = piece;
                int w = string_width(word);
                std::string bare = word;
                while (!bare.empty() && bare.back() == ' ')
                    bare.pop_back();
                if (used > 0 && used + string_width(bare) > width) {  // a trailing space may hang over the edge
                    rows.push_back(hbox(std::move(row)));
                    row.clear(), used = 0;
                    if (word.find_first_not_of(' ') == std::string::npos)
                        continue;
                }
                Element e = text(word) | color(base);
                if (span.bold)
                    e = e | bold;
                if (span.italic)
                    e = e | italic;
                if (span.link) {
                    links.emplace_back(Box{}, span.url);
                    e = e | underlined | color(T.link) | hyperlink(span.url) | reflect(links.back().first);
                }
                row.push_back(e);
                used += w;
            }
            a = b;
        }
    }
    if (!row.empty())
        rows.push_back(hbox(std::move(row)));
    if (rows.empty())
        rows.push_back(text(""));  // a blank line keeps its height
    return vbox(std::move(rows));
}

class Browser {
public:
    Browser(const Account& account, kitty::Support images, std::string paste_command, bool mouse)
        : store_(account), notes_(store_), images_(images), paste_command_(std::move(paste_command)),
          screen_(ScreenInteractive::Fullscreen()) {
        screen_.TrackMouse(mouse);
    }

    int run() {
        if (images_.enabled())
            Terminal::SetColorSupport(Terminal::Color::TrueColor);
        status_ = "loading…";
        auto ui = CatchEvent(Renderer([this] { return render(); }), [this](Event e) { return on_event(e); });
        screen_.Post([this] { refresh(); });
        screen_.Loop(ui);
        return 0;
    }

private:
    Store store_;
    Notes notes_;
    kitty::Images images_;
    std::string paste_command_;
    ScreenInteractive screen_;

    // Where things were drawn in the last frame, for mouse hit-testing. Deques, so the boxes
    // reflect() writes into stay put while the frame is built.
    std::deque<Box> item_boxes_;
    Box list_box_, preview_box_, search_box_;
    LinkBoxes links_;
    std::deque<std::pair<Box, Event>> buttons_;
    int last_click_item_ = -1;
    std::chrono::steady_clock::time_point last_click_;

    std::vector<Note> all_;
    std::vector<size_t> shown_;  // indices into all_
    int selected_ = 0;
    std::string filter_;
    bool filtering_ = false;
    std::string status_;
    std::optional<unsigned long> pending_delete_;
    int scroll_ = 0;  // preview lines scrolled past
    std::map<std::string, std::optional<Attachment>> attachments_;

    const Note* current() const {
        return shown_.empty() ? nullptr : &all_[shown_[std::clamp(selected_, 0, static_cast<int>(shown_.size()) - 1)]];
    }

    void select(int i) {
        int was = selected_;
        selected_ = shown_.empty() ? 0 : std::clamp(i, 0, static_cast<int>(shown_.size()) - 1);
        if (selected_ != was) {
            scroll_ = 0;
            pending_delete_.reset();
        }
    }

    void count_status() {
        status_ = filter_.empty() ? std::to_string(all_.size()) + " notes"
                                  : std::to_string(shown_.size()) + " of " + std::to_string(all_.size()) + " notes";
    }

    void apply_filter(const std::string& keep_uuid = "") {
        std::string q = lower(filter_);
        shown_.clear();
        for (size_t i = 0; i < all_.size(); ++i)
            if (q.empty() || lower(all_[i].title).find(q) != std::string::npos || lower(all_[i].md).find(q) != std::string::npos)
                shown_.push_back(i);
        int idx = 0;
        for (size_t k = 0; k < shown_.size(); ++k)
            if (all_[shown_[k]].uuid == keep_uuid)
                idx = static_cast<int>(k);
        selected_ = idx, scroll_ = 0;
        count_status();
    }

    void refresh(const std::string& keep_uuid = "") {
        try {
            all_ = notes_.load();
        } catch (const std::exception& e) {
            status_ = std::string("load failed: ") + e.what();
            return;
        }
        attachments_.clear();
        apply_filter(keep_uuid);
    }

    std::optional<std::string> edit(const std::string& text, const std::optional<std::string>& original = std::nullopt) {
        std::optional<std::string> out;
        std::string error;
        screen_.WithRestoredIO([&] {
            try {
                out = edit_text(text, original);
            } catch (const std::exception& e) {
                error = e.what();
            }
        })();
        if (!error.empty())
            status_ = error;
        return out;
    }

    void save(const Note* note, const std::string& text) {
        std::string uuid = note ? note->uuid : "";
        try {
            notes_.save(note, text);
        } catch (const BothKept& e) {
            refresh(uuid);
            status_ = std::string("saved, but ") + e.what();
            return;
        } catch (const Conflict&) {
            auto kept = keep_unsaved(text);
            refresh(uuid);
            status_ = "changed on the server while you edited: not saved; your text is in " + kept.string();
            return;
        } catch (const std::exception& e) {
            auto kept = keep_unsaved(text);
            status_ = std::string("save failed: ") + e.what() + "; your text is in " + kept.string();
            return;
        }
        refresh(uuid);
        if (!note && !all_.empty())  // a new note: show it
            for (size_t k = 0; k < shown_.size(); ++k)
                if (all_[shown_[k]].title == title_of(text))
                    selected_ = static_cast<int>(k);
        status_ = "saved";
    }

    // The clipboard's image, saved to a file and added at the end of the note (a new note when
    // there is none); the editor opens so the [attachment:...] line can be moved or removed.
    void paste(const Note* n) {
        ClipboardImage img;
        try {
            img = clipboard_image(paste_command_);
        } catch (const std::exception& ex) {
            status_ = ex.what();
            return;
        }
        // A private directory, so the image can have a plain name ("Pasted image.png" on the phone).
        const char* run = std::getenv("XDG_RUNTIME_DIR");
        std::string dir = std::string(run && *run ? run : "/tmp") + "/jotmail-XXXXXX";
        if (!mkdtemp(dir.data())) {
            status_ = "could not save the pasted image";
            return;
        }
        std::string path = dir + "/Pasted image." + (img.type == "image/jpeg" ? "jpg" : img.type.substr(6));
        {
            std::ofstream f(path, std::ios::binary);
            f << img.bytes;
            if (!f) {
                status_ = "could not save the pasted image";
                return;
            }
        }
        std::string line = "[attachment:" + path + "]\n";
        bool attempted = false;
        if (n) {
            Note copy = *n;
            std::string base = copy.md.empty() || copy.md.ends_with("\n") ? copy.md : copy.md + "\n";
            if (auto text = edit(base + line, copy.md))
                attempted = true, save(&copy, *text);
        } else if (auto text = edit("# \n" + line); text && text->find_first_not_of("# \n") != std::string::npos) {
            attempted = true, save(nullptr, *text);
        }
        if (!attempted || status_ == "saved") {  // kept after a failed save, for the unsaved text that names it
            ::unlink(path.c_str());
            ::rmdir(dir.c_str());
        }
    }

    // Clicks select notes, press footer buttons, open links and focus the search bar; the wheel
    // scrolls the note under it or moves through the list. A double click edits a note.
    bool on_mouse(Event e) {
        const Mouse& m = e.mouse();
        if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
            int dir = m.button == Mouse::WheelDown ? 1 : -1;
            if (preview_box_.Contain(m.x, m.y))
                scroll_ = std::max(0, scroll_ + 3 * dir);
            else if (list_box_.Contain(m.x, m.y))
                select(selected_ + dir);
            return true;
        }
        if (m.button != Mouse::Left || m.motion != Mouse::Pressed)
            return false;
        if (search_box_.Contain(m.x, m.y)) {
            filtering_ = true;
            return true;
        }
        filtering_ = false;
        for (auto& [box, ev] : buttons_)
            if (box.Contain(m.x, m.y))
                return on_event(ev);
        for (auto& [box, url] : links_)
            if (box.Contain(m.x, m.y)) {
                open_url(url);
                return true;
            }
        if (list_box_.Contain(m.x, m.y))
            for (size_t k = 0; k < item_boxes_.size(); ++k)
                if (item_boxes_[k].Contain(m.x, m.y)) {
                    auto now = std::chrono::steady_clock::now();
                    bool twice = static_cast<int>(k) == last_click_item_ && now - last_click_ < std::chrono::milliseconds(400);
                    last_click_item_ = twice ? -1 : static_cast<int>(k), last_click_ = now;
                    select(static_cast<int>(k));
                    return twice ? on_event(Event::Character('e')) : true;
                }
        return true;
    }

    void open_url(const std::string& url) {
        const char* browser = std::getenv("BROWSER");
#ifdef __APPLE__
        std::string opener = browser && *browser ? browser : "open";
#else
        std::string opener = browser && *browser ? browser : "xdg-open";
#endif
        std::string quoted = "'";
        for (char c : url)
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        quoted += "'";
        std::system((opener + " " + quoted + " >/dev/null 2>&1 &").c_str());
        status_ = "opened " + url;
    }

    bool on_event(const Event& e) {
        if (e.is_mouse())
            return on_mouse(e);
        if (filtering_) {
            if (e == Event::Return || e == Event::ArrowDown) {
                filtering_ = false;
            } else if (e == Event::Escape) {
                filtering_ = false, filter_.clear();
                apply_filter(current() ? current()->uuid : "");
            } else if (e == Event::Backspace) {
                pop_utf8(filter_);
                apply_filter();
            } else if (e.is_character()) {
                filter_ += e.character();
                apply_filter();
            } else {
                return false;
            }
            return true;
        }

        const Note* n = current();
        if (e == Event::Character('q')) {
            screen_.Exit();
        } else if (e == Event::Character('j') || e == Event::ArrowDown) {
            select(selected_ + 1);
        } else if (e == Event::Character('k') || e == Event::ArrowUp) {
            select(selected_ - 1);
        } else if (e == Event::Character('g') || e == Event::Home) {
            select(0);
        } else if (e == Event::Character('G') || e == Event::End) {
            select(static_cast<int>(shown_.size()) - 1);
        } else if (e == Event::Character('J') || e == Event::PageDown || e == Event::CtrlD) {
            scroll_ += e == Event::Character('J') ? 3 : 15;
        } else if (e == Event::Character('K') || e == Event::PageUp || e == Event::CtrlU) {
            scroll_ = std::max(0, scroll_ - (e == Event::Character('K') ? 3 : 15));
        } else if (e == Event::Character('e') || e == Event::Return) {
            if (n) {
                Note copy = *n;
                if (auto text = edit(copy.md))
                    save(&copy, *text);
            }
        } else if (e == Event::Character('p')) {
            paste(n);
        } else if (e == Event::Character('n')) {
            if (auto text = edit("# \n"); text && text->find_first_not_of("# \n") != std::string::npos)
                save(nullptr, *text);
        } else if (e == Event::Character('d')) {
            if (!n)
                return true;
            if (pending_delete_ != n->uid) {
                pending_delete_ = n->uid;
                status_ = "press d again to delete “" + n->title + "” (it goes to Trash)";
                return true;
            }
            pending_delete_.reset();
            std::string title = n->title;
            try {
                notes_.remove(*n);
            } catch (const std::exception& ex) {
                status_ = std::string("delete failed: ") + ex.what();
                return true;
            }
            refresh();
            status_ = "deleted “" + title + "”";
        } else if (e == Event::Character('/')) {
            filtering_ = true;
        } else if (e == Event::Character('r')) {
            status_ = "loading…";
            refresh(n ? n->uuid : "");
        } else if (e == Event::Escape) {
            if (!filter_.empty()) {
                filter_.clear();
                apply_filter(n ? n->uuid : "");
            }
        } else {
            return false;
        }
        return true;
    }

    // --- drawing ------------------------------------------------------------------

    Element render() {
        item_boxes_.clear(), links_.clear(), buttons_.clear();
        Element bar = text(" Notes · " + store_.account().user + " · " + status_) | color(T.status) | bgcolor(T.panel);
        Element search = filtering_    ? hbox(text(" /" + filter_) | color(T.text), text("▏") | color(T.accent))
                         : filter_.empty() ? text(" / search notes") | color(T.muted)
                                           : text(" /" + filter_) | color(T.text);
        search = search | bgcolor(filtering_ ? T.highlight : T.search) | reflect(search_box_);

        Elements items;
        for (size_t k = 0; k < shown_.size(); ++k) {
            const Note& n = all_[shown_[k]];
            bool sel = static_cast<int>(k) == selected_;
            Element item = vbox({
                text(" " + fit(n.title + (n.attachments ? " 📎" : ""), LIST_WIDTH - 2)) | bold | color(sel ? T.selected_text : T.text),
                text(" " + when(n.date)) | color(sel ? T.selected_text : T.muted),
            });
            item_boxes_.emplace_back();
            item = item | bgcolor(sel ? T.accent : T.panel) | reflect(item_boxes_.back());
            if (sel)
                item = item | focus;
            items.push_back(item);
        }
        if (items.empty())
            items.push_back(text(""));
        Element list = vbox(std::move(items)) | yframe | vscroll_indicator | size(WIDTH, EQUAL, LIST_WIDTH) | bgcolor(T.panel) |
                       reflect(list_box_);

        // the footer's hints are buttons too
        Elements hints{text(" ")};
        for (auto [label, key] : {std::pair{"e edit", 'e'}, {"n new", 'n'}, {"p paste image", 'p'}, {"d delete", 'd'},
                                  {"/ search", '/'}, {"r refresh", 'r'}, {"q quit", 'q'}}) {
            buttons_.emplace_back(Box{}, Event::Character(key));
            hints.push_back(text(label) | reflect(buttons_.back().first));
            hints.push_back(text(" · "));
        }
        hints.push_back(text("J/K scroll"));
        Element footer = hbox(std::move(hints)) | color(T.muted) | bgcolor(T.panel);
        Element note = preview() | flex | reflect(preview_box_);
        return vbox({bar, search, hbox({list, separator() | color(T.highlight), note}) | flex, footer}) | bgcolor(T.background);
    }

    Element preview() {
        const Note* n = current();
        if (!n)
            return paragraph(" No notes. Press n to write one.") | color(T.muted);

        auto dim = Terminal::Size();
        int width = std::max(10, dim.dimx - LIST_WIDTH - 5);
        int max_rows = std::max(4, (dim.dimy - 6) * 2 / 3);

        Elements rows;
        size_t a = 0;
        const std::string& md = n->md;
        while (a < md.size()) {
            size_t e = md.find('\n', a);
            std::string line = md.substr(a, e == std::string::npos ? std::string::npos : e - a);
            a = e == std::string::npos ? md.size() : e + 1;

            if (line.starts_with("[attachment:") && line.find(']') != std::string::npos) {
                std::string cid = line.substr(12, line.find(']') - 12);
                if (auto img = image(*n, cid, width, max_rows)) {
                    Color id = Color::RGB(img->id >> 16 & 0xFF, img->id >> 8 & 0xFF, img->id & 0xFF);
                    for (int r = 0; r < img->rows; ++r)
                        rows.push_back(text(kitty::placeholder_row(r, img->cols)) | color(id));
                } else {
                    rows.push_back(text("📎 image") | italic | color(T.muted));
                }
                continue;
            }
            size_t h = 0;
            while (h < line.size() && h < 3 && line[h] == '#')
                ++h;
            if (h > 0 && h < line.size() && line[h] == ' ') {
                rows.push_back(styled(line.substr(h + 1), h == 1 ? T.accent : T.link, width, links_) | bold);
                continue;
            }
            size_t indent = line.find_first_not_of(' ');
            if (indent != std::string::npos && (line.compare(indent, 2, "- ") == 0 || line.compare(indent, 2, "* ") == 0)) {
                rows.push_back(hbox({text("  • ") | color(T.accent), styled(line.substr(indent + 2), T.text, width - 4, links_) | flex}));
                continue;
            }
            if (indent != std::string::npos) {
                size_t d = indent;
                while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d])))
                    ++d;
                if (d > indent && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ') {
                    rows.push_back(hbox({text("  " + line.substr(indent, d - indent + 1) + " ") | color(T.accent),
                                         styled(line.substr(d + 2), T.text, width - 6, links_) | flex}));
                    continue;
                }
            }
            rows.push_back(styled(line, T.text, width, links_));
        }
        scroll_ = std::min(scroll_, std::max(0, static_cast<int>(rows.size()) - 1));
        Elements visible(rows.begin() + scroll_, rows.end());
        // yframe: a note taller than the pane is clipped at the bottom instead of having its lines squeezed
        return hbox({text("  "), vbox(std::move(visible)) | yframe | flex, text(" ")}) | bgcolor(T.background);
    }

    std::optional<kitty::Placed> image(const Note& n, const std::string& cid, int max_cols, int max_rows) {
        if (!images_.enabled() || n.raw.empty())
            return std::nullopt;
        std::string key = n.uuid + "/" + std::to_string(n.uid) + "/" + cid;
        auto it = attachments_.find(key);
        if (it == attachments_.end())
            it = attachments_.emplace(key, find_attachment(n.raw, cid)).first;
        if (!it->second)
            return std::nullopt;
        return images_.place(key, it->second->type, it->second->bytes, max_cols, max_rows);
    }
};

}  // namespace

int run_tui(const Account& account, const Config& config) {
    T = make_theme(config.theme_preset, config.theme_colors);
    Browser browser(account, kitty::detect(config.images), config.paste_command, config.mouse);
    return browser.run();
}

}  // namespace jotmail
