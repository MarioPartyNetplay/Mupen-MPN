Name:    RMG       
Version: 0.8.9
Release: %autorelease
Summary: Mupen MPN 

License: GPL-3.0-only       
URL:     https://github.com/Rosalie241/RMG
Source0: https://github.com/Rosalie241/RMG/archive/refs/tags/v%{version}.tar.gz       

BuildRequires: gcc
BuildRequires: g++
BuildRequires: nasm
BuildRequires: cmake
BuildRequires: libusb1-devel
BuildRequires: hidapi-devel
BuildRequires: libsamplerate-devel
BuildRequires: minizip-compat-devel
BuildRequires: SDL3-devel
BuildRequires: freetype-devel
BuildRequires: mesa-libGL-devel
BuildRequires: mesa-libGLU-devel
BuildRequires: zlib-ng-devel
BuildRequires: binutils-devel
BuildRequires: speexdsp-devel
BuildRequires: qt6-qtbase-devel
BuildRequires: qt6-qtsvg-devel
BuildRequires: qt6-qtwebsockets-devel
BuildRequires: libxkbcommon-devel
BuildRequires: libatomic

Requires: libusb1
Requires: hidapi
Requires: SDL3
Requires: zlib-ng
Requires: libsamplerate
Requires: speexdsp
Requires: qt6-qtbase
Requires: qt6-qtsvg
Requires: qt6-qtwebsockets
Requires: libatomic

%description
Mupen MPN is a free and open-source mupen64plus front-end written in C++

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE_INSTALL=OFF
%cmake_build

%install
%cmake_install

%files
/usr/bin/RMG
/usr/lib64/libRMG-Core.so
/usr/lib64/RMG/
/usr/share/RMG/
/usr/share/applications/org.marioparty.mupen.desktop
/usr/share/icons/hicolor/512x512/apps/org.marioparty.mupen.png
/usr/share/metainfo/org.marioparty.mupen.metainfo.xml

%changelog
%autochangelog
