// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include "store.hpp"

namespace jotmail {

struct Note {
    unsigned long uid = 0;
    std::string uuid;     // X-Universally-Unique-Identifier, or "no-uuid-<uid>"
    std::string title;    // the Subject
    std::time_t date = 0;
    std::string created;  // X-Mail-Created-Date, kept across edits
    std::string md;
    bool attachments = false;
    std::string raw;      // the whole message, kept for notes with attachments
};

// The note changed on the server since it was loaded.
class Conflict : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One note per UUID (the newest UID wins), newest first.
std::vector<Note> parse_notes(const std::vector<RawNote>& raw);
Note parse_note(const RawNote& raw);

// A new text/html note as iOS writes it.
std::string build_note(const std::string& md, const std::string& uuid, const std::string& created,
                       const std::string& from, std::time_t now);

// The multipart note `raw` with its text replaced by `md`. Images whose [attachment:<cid>]
// line is gone are dropped; the rest are kept byte for byte.
std::string rebuild_note(const std::string& raw, const std::string& md, const std::string& domain, std::time_t now);

std::string new_uuid();  // uppercase v4, as iOS writes

class Notes {
public:
    explicit Notes(Store& store) : store_(store) {}

    std::vector<Note> load();

    // Saves `md` as a new version of `note` (or as a new note when null): appends the new version
    // with the same UUID, then removes the old one, so the phone sees an edit, not a new note.
    // Throws Conflict, saving nothing, if the note changed on the server since it was loaded.
    void save(const Note* note, const std::string& md);

    // Moves the note, and any older copies of it, to Trash.
    void remove(const Note& note);

private:
    Store& store_;
};

}  // namespace jotmail
