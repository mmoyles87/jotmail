// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace jotmail {

struct Account {
    std::string name;
    std::string host;
    int port = 993;
    std::string user;
    std::string password_command;  // run with /bin/sh; the first line of its output is the password
    std::string folder = "Notes";
};

struct Config {
    std::string default_account;
    std::map<std::string, Account> accounts;

    // $XDG_CONFIG_HOME/jotmail/config.toml, or ~/.config/jotmail/config.toml
    static std::filesystem::path default_path();
    static Config load(const std::filesystem::path& path);

    // The named account, or the default one when name is empty.
    const Account& account(const std::string& name) const;
};

std::string run_password_command(const std::string& command);

}  // namespace jotmail
