// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include <toml++/toml.hpp>

namespace jotmail {

std::filesystem::path Config::default_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "jotmail/config.toml";
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("HOME is not set");
    return std::filesystem::path(home) / ".config/jotmail/config.toml";
}

Config Config::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path))
        throw std::runtime_error("no config at " + path.string() + " (see README for the format)");

    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(path.string() + ": " + std::string(e.description()));
    }

    Config c;
    c.default_account = t["default"].value_or(std::string{});
    auto* accounts = t["accounts"].as_table();
    if (!accounts || accounts->empty())
        throw std::runtime_error(path.string() + ": no [accounts.<name>] sections");

    for (auto& [key, node] : *accounts) {
        auto* a = node.as_table();
        if (!a)
            continue;
        Account acc;
        acc.name = std::string(key.str());
        auto need = [&](const char* field) {
            auto v = (*a)[field].value<std::string>();
            if (!v)
                throw std::runtime_error(path.string() + ": accounts." + acc.name + " is missing " + field);
            return *v;
        };
        acc.host = need("host");
        acc.user = need("user");
        acc.password_command = need("password_command");
        acc.port = (*a)["port"].value_or(993);
        acc.folder = (*a)["folder"].value_or(std::string("Notes"));
        c.accounts.emplace(acc.name, std::move(acc));
    }
    if (c.default_account.empty())
        c.default_account = c.accounts.begin()->first;
    return c;
}

const Account& Config::account(const std::string& name) const {
    const std::string& want = name.empty() ? default_account : name;
    auto it = accounts.find(want);
    if (it == accounts.end())
        throw std::runtime_error("no account named \"" + want + "\" in the config");
    return it->second;
}

std::string run_password_command(const std::string& command) {
    FILE* p = popen(command.c_str(), "r");
    if (!p)
        throw std::runtime_error("could not run password_command");
    std::string out;
    char buf[256];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        out.append(buf, n);
    int status = pclose(p);
    if (status != 0)
        throw std::runtime_error("password_command failed (exit status " + std::to_string(WEXITSTATUS(status)) + ")");
    if (auto nl = out.find('\n'); nl != std::string::npos)
        out.resize(nl);
    if (out.empty())
        throw std::runtime_error("password_command printed nothing");
    return out;
}

}  // namespace jotmail
