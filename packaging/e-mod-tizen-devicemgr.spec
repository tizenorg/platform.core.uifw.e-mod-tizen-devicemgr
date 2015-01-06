Name: e-mod-tizen-devicemgr
Version: 0.0.1
Release: 1
Summary: The devicemgr for enlightenment modules
URL: http://www.enlightenment.org
Group: Graphics & UI Framework/Other
Source0: %{name}-%{version}.tar.gz
License: BSD-2-Clause
BuildRequires: pkgconfig(enlightenment)
BuildRequires: pkgconfig(elementary)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(x11)
BuildRequires: pkgconfig(xextproto)
BuildRequires: pkgconfig(xfixes)
BuildRequires: pkgconfig(xext)
BuildRequires: pkgconfig(xrandr)
BuildRequires: pkgconfig(evas)
BuildRequires: pkgconfig(xi)
BuildRequires: pkgconfig(xtst)
BuildRequires: pkgconfig(utilX)
Requires: libX11

%description
This package is a devicemgr for enlightenment.

%prep
%setup -q

%build

export GC_SECTIONS_FLAGS="-fdata-sections -ffunction-sections -Wl,--gc-sections"
export CFLAGS+=" -Wall -Werror -g -fPIC -rdynamic ${GC_SECTIONS_FLAGS}"
export LDFLAGS+=" -Wl,--hash-style=both -Wl,--as-needed -Wl,--rpath=/usr/lib"

%autogen
%configure --prefix=/usr
make

%install
rm -rf %{buildroot}

# for license notification
mkdir -p %{buildroot}/usr/share/license
cp -a %{_builddir}/%{buildsubdir}/COPYING %{buildroot}/usr/share/license/%{name}

# install
make install DESTDIR=%{buildroot}

# clear useless textual files
find  %{buildroot}/usr/lib/enlightenment/modules/%{name} -name *.la | xargs rm

%files
%defattr(-,root,root,-)
%{_libdir}/enlightenment/modules/e-mod-tizen-devicemgr
/usr/share/license/%{name}
