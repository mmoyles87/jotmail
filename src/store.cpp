// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.hpp"

#include <arpa/inet.h>

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <vmime/net/imap/IMAPCommand.hpp>
#include <vmime/net/imap/IMAPConnection.hpp>
#include <vmime/net/imap/IMAPFolder.hpp>
#include <vmime/net/imap/IMAPFolderStatus.hpp>
#include <vmime/net/imap/IMAPParser.hpp>
#include <vmime/net/imap/IMAPSearchAttributes.hpp>
#include <vmime/net/imap/IMAPStore.hpp>
#include <vmime/net/imap/IMAPUtils.hpp>
#include <vmime/platforms/posix/posixHandler.hpp>
#include <vmime/security/cert/certificateException.hpp>
#include <vmime/security/cert/certificateVerifier.hpp>
#include <vmime/vmime.hpp>

namespace jotmail {

namespace {

const char* CA_BUNDLES[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Arch, Debian, Ubuntu, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
    "/etc/ssl/cert.pem",                   // macOS, BSDs
    "/opt/homebrew/etc/ca-certificates/cert.pem",
    "/usr/local/etc/ca-certificates/cert.pem",
};

// Verifies the server's certificate chain with OpenSSL itself: every signature up to a trusted
// root, validity dates, the TLS server purpose, and the host name (RFC 6125, via
// X509_VERIFY_PARAM_set1_host). vmime's own default verifier only checked issuer names between
// links and signatures on the last one, which a forged leaf could get past.
class OpenSslVerifier : public vmime::security::cert::certificateVerifier {
public:
    explicit OpenSslVerifier(std::string ca_file) : ca_file_(std::move(ca_file)) {}

    void verify(const std::shared_ptr<vmime::security::cert::certificateChain>& chain, const std::string& hostname) override {
        using vmime::security::cert::certificateException;
        if (!chain || chain->getCount() == 0)
            throw certificateException("the server sent no certificate");

        std::vector<X509*> certs;
        auto free_all = [&] {
            for (X509* c : certs)
                X509_free(c);
        };
        for (size_t i = 0; i < chain->getCount(); ++i) {
            const vmime::byteArray der = chain->getAt(i)->getEncoded();
            const unsigned char* p = der.data();
            X509* c = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
            if (!c) {
                free_all();
                throw certificateException("the server sent an unreadable certificate");
            }
            certs.push_back(c);
        }

        X509_STORE* store = X509_STORE_new();
        STACK_OF(X509)* untrusted = sk_X509_new_null();
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        auto cleanup = [&] {
            X509_STORE_CTX_free(ctx);
            sk_X509_free(untrusted);  // the certificates themselves are freed by free_all
            X509_STORE_free(store);
            free_all();
        };
        bool roots = X509_STORE_set_default_paths(store) == 1;
        for (const char* path : CA_BUNDLES)
            if (std::filesystem::exists(path) && X509_STORE_load_file(store, path) == 1) {
                roots = true;
                break;
            }
        if (!ca_file_.empty() && X509_STORE_load_file(store, ca_file_.c_str()) != 1) {
            cleanup();
            throw certificateException("no certificates in ca_file " + ca_file_);
        }
        if (!roots && ca_file_.empty()) {
            cleanup();
            throw certificateException("no CA certificate bundle found");
        }
        for (size_t i = 1; i < certs.size(); ++i)
            sk_X509_push(untrusted, certs[i]);

        int ok = X509_STORE_CTX_init(ctx, store, certs[0], untrusted);
        if (ok == 1) {
            X509_STORE_CTX_set_purpose(ctx, X509_PURPOSE_SSL_SERVER);
            X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(ctx);
            unsigned char ip[16];
            bool is_ip = inet_pton(AF_INET, hostname.c_str(), ip) == 1 || inet_pton(AF_INET6, hostname.c_str(), ip) == 1;
            ok = is_ip ? X509_VERIFY_PARAM_set1_ip_asc(param, hostname.c_str())
                       : X509_VERIFY_PARAM_set1_host(param, hostname.c_str(), hostname.size());
            if (ok == 1)
                ok = X509_verify_cert(ctx);
        }
        std::string why = ok == 1 ? "" : X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx));
        cleanup();
        if (ok != 1)
            throw certificateException("the server's certificate for " + hostname + " is not trusted: " + why);
    }

private:
    std::string ca_file_;
};

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

    impl_->store->setCertificateVerifier(std::make_shared<OpenSslVerifier>(account_.ca_file));

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
    loaded_validity_ = uid_validity();
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

unsigned long Store::uid_validity() {
    connect();
    auto status = vmime::dynamicCast<imap::IMAPFolderStatus>(impl_->folder->getStatus());
    if (!status)
        throw std::runtime_error("the server did not report the folder's UIDVALIDITY");
    return status->getUIDValidity();
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
    if (impl_->conn()->hasCapability("UIDPLUS"))  // otherwise left marked: see store.hpp
        impl_->command("UID EXPUNGE " + set);
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
