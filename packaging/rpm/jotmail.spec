%global ftxui_version 7.0.3
%global toml_version 3.4.0
%global stb_commit 2c980bb59875b0d32144a71867fbdebb2f77cd20
%global vmime_commit 63ddb3b378487e7209ec3390cb33c6cde9bdec0a

Name:           jotmail
Version:        0.1.0
Release:        1%{?dist}
Summary:        Notes from your IMAP mailbox, in a terminal
License:        GPL-3.0-or-later AND MIT
URL:            https://github.com/mmoyles87/jotmail
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
Source1:        https://github.com/ArthurSonzogni/FTXUI/archive/v%{ftxui_version}/FTXUI-%{ftxui_version}.tar.gz
Source2:        https://github.com/marzer/tomlplusplus/archive/v%{toml_version}/tomlplusplus-%{toml_version}.tar.gz
Source3:        https://github.com/nothings/stb/archive/%{stb_commit}/stb-%{stb_commit}.tar.gz
Source4:        https://github.com/kisli/vmime/archive/%{vmime_commit}/vmime-%{vmime_commit}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.28
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  gtest-devel

Recommends:     (wl-clipboard or xclip)

Provides:       bundled(ftxui) = %{ftxui_version}
Provides:       bundled(tomlplusplus) = %{toml_version}
Provides:       bundled(stb_image)
Provides:       bundled(vmime)

%description
Apple Notes, Fastmail's web Notes and ImapNotes3 keep each note as an HTML
message in a mail account's Notes folder. jotmail reads and writes that folder
over IMAP, so Apple Notes sees an edit made in jotmail as an edit, not a
duplicate.

It has a two-pane terminal browser with search, edits notes as Markdown in
your editor, shows images inline in kitty and Ghostty, and has a command line
for scripts.

%prep
%autosetup
# %%autosetup takes only one -a
tar xf %{SOURCE1}
tar xf %{SOURCE2}
tar xf %{SOURCE3}
tar xf %{SOURCE4}

%build
%cmake -G Ninja \
    -DBUILD_SHARED_LIBS=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_FTXUI=$PWD/FTXUI-%{ftxui_version} \
    -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$PWD/tomlplusplus-%{toml_version} \
    -DFETCHCONTENT_SOURCE_DIR_STB=$PWD/stb-%{stb_commit} \
    -DFETCHCONTENT_SOURCE_DIR_VMIME=$PWD/vmime-%{vmime_commit}
%cmake_build

%check
%ctest

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_bindir}/jotmail
%{_mandir}/man1/jotmail.1*

%changelog
* Mon Oct 26 2026 Matt Moyles <matt@mattmoyles.com> - 0.1.0-1
- Initial release
