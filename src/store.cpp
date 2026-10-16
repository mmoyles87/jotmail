// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

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

struct Store::Impl {
    std::shared_ptr<vmime::net::session> session;
    std::shared_ptr<vmime::net::store> store;
    std::shared_ptr<vmime::net::folder> folder;
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
}

std::vector<RawNote> Store::fetch_all() {
    connect();
    auto& f = impl_->folder;
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

}  // namespace jotmail
