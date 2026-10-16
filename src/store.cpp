// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <vmime/net/imap/IMAPCommand.hpp>
#include <vmime/net/imap/IMAPConnection.hpp>
#include <vmime/net/imap/IMAPFolder.hpp>
#include <vmime/net/imap/IMAPParser.hpp>
#include <vmime/net/imap/IMAPSearchAttributes.hpp>
#include <vmime/net/imap/IMAPStore.hpp>
#include <vmime/net/imap/IMAPUtils.hpp>
#include <vmime/platforms/posix/posixHandler.hpp>
#include <vmime/security/cert/defaultCertificateVerifier.hpp>
#include <vmime/vmime.hpp>

namespace jotmail {

namespace {

// The system's trusted roots. vmime verifies against an explicit list, not the OS store.
std::vector<std::shared_ptr<vmime::security::cert::X509Certificate>> system_roots() {
    static const char* bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",  // Arch, Debian, Ubuntu, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
        "/etc/ssl/cert.pem",                   // macOS, BSDs
        "/opt/homebrew/etc/ca-certificates/cert.pem",
        "/usr/local/etc/ca-certificates/cert.pem",
    };
    std::vector<std::shared_ptr<vmime::security::cert::X509Certificate>> certs;
    for (const char* path : bundles) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            continue;
        std::string pem((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        vmime::security::cert::X509Certificate::import(
            reinterpret_cast<const vmime::byte_t*>(pem.data()), pem.size(), certs);
        if (!certs.empty())
            return certs;
    }
    throw std::runtime_error("no CA certificate bundle found");
}

}  // namespace

namespace imap = vmime::net::imap;

struct Store::Impl {
    std::shared_ptr<vmime::net::session> session;
    std::shared_ptr<vmime::net::store> store;
    std::shared_ptr<vmime::net::folder> folder;
    bool stale = false;  // written to over the other connection since the folder was opened

    // vmime gives each open folder its own connection and keeps it private, so commands
    // vmime has no API for (UID EXPUNGE, UID MOVE) go over the store's connection, which
    // selects the same folder.
    std::shared_ptr<imap::IMAPConnection> conn() {
        return vmime::dynamicCast<imap::IMAPStore>(store)->getConnection();
    }

    std::string mailbox(const vmime::net::folder::path& path) {
        return imap::IMAPUtils::quoteString(imap::IMAPUtils::pathToString(conn()->hierarchySeparator(), path));
    }

    void command(const std::string& text) {
        auto c = conn();
        imap::IMAPCommand::createCommand(text)->send(c);
        std::unique_ptr<imap::IMAPParser::response> resp(c->readResponse());
        if (resp->isBad() || resp->response_done->response_tagged->resp_cond_state->status != imap::IMAPParser::resp_cond_state::OK)
            throw std::runtime_error("the server refused " + text.substr(0, text.find(' ', 4)) + ": " + resp->getErrorLog());
    }
};

Store::Store(Account account) : account_(std::move(account)), impl_(std::make_unique<Impl>()) {
    static bool platform_ready = [] {
        vmime::platform::setHandler<vmime::platforms::posix::posixHandler>();
        return true;
    }();
    (void)platform_ready;
}

Store::~Store() {
    try {
        if (impl_->folder && impl_->folder->isOpen())
            impl_->folder->close(false);
        if (impl_->store && impl_->store->isConnected())
            impl_->store->disconnect();
    } catch (...) {
    }
}

void Store::connect() {
    if (impl_->store && impl_->store->isConnected() && impl_->folder && impl_->folder->isOpen()) {
        try {
            impl_->store->noop();
            return;
        } catch (const vmime::exception&) {
        }
    }

    impl_->session = vmime::net::session::create();
    vmime::utility::url url("imaps", account_.host, account_.port);
    impl_->store = impl_->session->getStore(url);
    impl_->store->setProperty("auth.username", account_.user);
    impl_->store->setProperty("auth.password", run_password_command(account_.password_command));

    auto verifier = std::make_shared<vmime::security::cert::defaultCertificateVerifier>();
    verifier->setX509RootCAs(system_roots());
    impl_->store->setCertificateVerifier(verifier);

    impl_->store->connect();

    vmime::net::folder::path path(vmime::net::folder::path::component(account_.folder));
    impl_->folder = impl_->store->getFolder(path);
    if (!impl_->folder->exists()) {
        vmime::net::folderAttributes attrs;
        attrs.setType(vmime::net::folderAttributes::TYPE_CONTAINS_MESSAGES);
        impl_->folder->create(attrs);
    }
    impl_->folder->open(vmime::net::folder::MODE_READ_WRITE);
    impl_->command("SELECT " + impl_->mailbox(path));
}

std::vector<RawNote> Store::fetch_all() {
    connect();
    auto& f = impl_->folder;
    if (impl_->stale) {
        f->close(false);
        f->open(vmime::net::folder::MODE_READ_WRITE);
        impl_->stale = false;
    }
    std::vector<RawNote> out;
    if (f->getMessageCount() == 0)
        return out;

    auto msgs = f->getMessages(vmime::net::messageSet::byNumber(1, -1));
    f->fetchMessages(msgs, vmime::net::fetchAttributes(vmime::net::fetchAttributes::UID));
    out.reserve(msgs.size());
    for (auto& m : msgs) {
        std::ostringstream buf;
        vmime::utility::outputStreamAdapter os(buf);
        m->extract(os, nullptr, 0, static_cast<size_t>(-1), true);
        out.push_back({m->getUID(), buf.str()});
    }
    return out;
}

std::vector<unsigned long> Store::search_header(const std::string& header, const std::string& value) {
    connect();
    imap::IMAPSearchAttributes sa;
    sa.add(imap::IMAPSearchTokenFactory::HEADER(header.c_str(), value));
    auto f = vmime::dynamicCast<imap::IMAPFolder>(impl_->folder);
    std::vector<unsigned long> out;
    for (auto& uid : f->getMessageUIDsMatchingSearchAttributes(sa))
        out.push_back(std::stoul(uid));
    return out;
}

void Store::append(const std::string& raw) {
    connect();
    vmime::utility::inputStreamStringAdapter is(raw);
    vmime::datetime now = vmime::datetime::now();
    impl_->folder->addMessage(is, raw.size(), vmime::net::message::FLAG_SEEN, &now);
    impl_->stale = true;
}

namespace {

std::string uid_set(const std::vector<unsigned long>& uids) {
    std::string s;
    for (auto u : uids)
        s += (s.empty() ? "" : ",") + std::to_string(u);
    return s;
}

}  // namespace

void Store::remove(const std::vector<unsigned long>& uids) {
    if (uids.empty())
        return;
    connect();
    std::string set = uid_set(uids);
    impl_->command("UID STORE " + set + " +FLAGS.SILENT (\\Deleted)");
    impl_->command(impl_->conn()->hasCapability("UIDPLUS") ? "UID EXPUNGE " + set : "EXPUNGE");
    impl_->stale = true;
}

void Store::trash(const std::vector<unsigned long>& uids) {
    if (uids.empty())
        return;
    connect();
    std::optional<vmime::net::folder::path> trash;
    for (auto& f : impl_->store->getRootFolder()->getFolders(true))
        if (f->getAttributes().getSpecialUse() == vmime::net::folderAttributes::SPECIALUSE_TRASH) {
            trash = f->getFullPath();
            break;
        }
    if (!trash)
        throw std::runtime_error("the account has no Trash folder (none is marked \\Trash)");
    std::string set = uid_set(uids), to = impl_->mailbox(*trash);
    if (impl_->conn()->hasCapability("MOVE")) {
        impl_->command("UID MOVE " + set + " " + to);
        impl_->stale = true;
    } else {
        impl_->command("UID COPY " + set + " " + to);
        remove(uids);
    }
}

}  // namespace jotmail
