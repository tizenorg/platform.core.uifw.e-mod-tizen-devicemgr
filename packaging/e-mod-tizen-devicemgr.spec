%bcond_with x
%bcond_with wayland

Name: e-mod-tizen-devicemgr
Version: 0.1.10
Release: 1
Summary: The devicemgr for enlightenment modules
URL: http://www.enlightenment.org
Group: Graphics & UI Framework/Other
Source0: %{name}-%{version}.tar.gz
License: BSD-2-Clause
BuildRequires: pkgconfig(enlightenment)
BuildRequires: pkgconfig(elementary)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(cairo)
BuildRequires: pkgconfig(ttrace)
%if %{with x}
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
%endif

%if %{with wayland}
BuildRequires: pkgconfig(libtbm)
BuildRequires: pkgconfig(libtdm)
BuildRequires: pkgconfig(libpng)
BuildRequires: pkgconfig(pixman-1)
BuildRequires: pkgconfig(wayland-server)
BuildRequires: pkgconfig(screenshooter-server)
BuildRequires: pkgconfig(tizen-extension-server)
BuildRequires: pkgconfig(wayland-tbm-server)
%endif

%description
This package is a devicemgr for enlightenment.

%prep
%setup -q

%build

export GC_SECTIONS_FLAGS="-fdata-sections -ffunction-sections -Wl,--gc-sections"
export CFLAGS+=" -Wall -Werror -g -fPIC -rdynamic ${GC_SECTIONS_FLAGS} -DE_LOGGING=1"
export LDFLAGS+=" -Wl,--hash-style=both -Wl,--as-needed -Wl,--rpath=/usr/lib"

%if %{with wayland}
%reconfigure --enable-wayland-only
%else
%reconfigure 
%endif

make

%install
rm -rf %{buildroot}

# for license notification
mkdir -p %{buildroot}/usr/share/license
cp -a %{_builddir}/%{buildsubdir}/COPYING %{buildroot}/usr/share/license/%{name}

# install
make install DESTDIR=%{buildroot}

# clear useless textual files
find  %{buildroot}%{_libdir}/enlightenment/modules/%{name} -name *.la | xargs rm

%files
%defattr(-,root,root,-)
%{_libdir}/enlightenment/modules/e-mod-tizen-devicemgr
/usr/share/license/%{name}
