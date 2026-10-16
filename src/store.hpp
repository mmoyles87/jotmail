// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config.hpp"

namespace jotmail {

// One message in the notes folder, exactly as the server holds it.
struct RawNote {
    std::string uid;
    std::string bytes;
};

// The account's notes folder over IMAP.
class Store {
public:
    explicit Store(Account account);
    ~Store();

    // Every message in the folder, in UID order. Fetched with BODY.PEEK, so flags stay as they are.
    std::vector<RawNote> fetch_all();

    const Account& account() const { return account_; }

private:
    struct Impl;
    Account account_;
    std::unique_ptr<Impl> impl_;
    void connect();
};

}  // namespace jotmail
