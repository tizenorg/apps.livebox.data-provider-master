Name: org.tizen.data-provider-master
Summary: Master service provider for liveboxes.
Version: 0.15.12
Release: 1
Group: framework/livebox
License: Flora License
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake, gettext-tools, smack, coreutils
BuildRequires: pkgconfig(ail)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(aul)
BuildRequires: pkgconfig(vconf)
BuildRequires: pkgconfig(sqlite3)
BuildRequires: pkgconfig(db-util)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(gio-2.0)
BuildRequires: pkgconfig(bundle)
BuildRequires: pkgconfig(ecore-x)
BuildRequires: pkgconfig(ecore)
BuildRequires: pkgconfig(evas)
BuildRequires: pkgconfig(ecore-evas)
BuildRequires: pkgconfig(com-core)
BuildRequires: pkgconfig(heynoti)
BuildRequires: pkgconfig(libxml-2.0)
BuildRequires: pkgconfig(x11)
BuildRequires: pkgconfig(libdri2)
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(libdrm_slp)
BuildRequires: pkgconfig(xfixes)
BuildRequires: pkgconfig(dri2proto)
BuildRequires: pkgconfig(xext)
BuildRequires: pkgconfig(xdamage)
BuildRequires: pkgconfig(pkgmgr)
BuildRequires: pkgconfig(livebox-service)

%description
Manage the 2nd stage livebox service provider and communicate with the viewer application.
Keep trace on the life-cycle of the livebox and status of the service providers, viewer applications.

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix}
CFLAGS="${CFLAGS} -Wall -Winline -Werror" LDFLAGS="${LDFLAGS}" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datarootdir}/license
mkdir -p %{buildroot}/opt/usr/share/live_magazine
mkdir -p %{buildroot}/opt/usr/share/live_magazine/log
mkdir -p %{buildroot}/opt/usr/share/live_magazine/reader
mkdir -p %{buildroot}/opt/usr/share/live_magazine/always
mkdir -p %{buildroot}/opt/dbspace
mkdir -p %{buildroot}/%{_sysconfdir}/rc.d/rc3.d
mkdir -p %{buildroot}/%{_libdir}/systemd/user/tizen-middleware.target.wants
touch %{buildroot}/opt/dbspace/.livebox.db
touch %{buildroot}/opt/dbspace/.livebox.db-journal
ln -sf %{_sysconfdir}/rc.d/init.d/data-provider-master %{buildroot}/%{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
ln -sf %{_libdir}/systemd/user/data-provider-master.service %{buildroot}/%{_libdir}/systemd/user/tizen-middleware.target.wants/data-provider-master.service

%pre
# Executing the stop script for stopping the service of installed provider (old version)
if [ -x %{_sysconfdir}/rc.d/init.d/data-provider-master ]; then
	%{_sysconfdir}/rc.d/init.d/data-provider-master stop
fi

%post
echo "Successfully installed. Please start a daemon again manually"
echo "%{_sysconfdir}/init.d/data-provider-master start"

%files -n org.tizen.data-provider-master
%manifest org.tizen.data-provider-master.manifest
%defattr(-,root,root,-)
%{_sysconfdir}/rc.d/init.d/data-provider-master
%{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
%{_bindir}/data-provider-master
%{_bindir}/liveinfo
%{_prefix}/etc/package-manager/parserlib/*
%{_datarootdir}/data-provider-master/*
%{_libdir}/systemd/user/data-provider-master.service
%{_libdir}/systemd/user/tizen-middleware.target.wants/data-provider-master.service
%{_datarootdir}/license/*
%attr(750,app,app) /opt/usr/share/live_magazine
%attr(750,app,app) /opt/usr/share/live_magazine/log
%attr(750,app,app) /opt/usr/share/live_magazine/reader
%attr(750,app,app) /opt/usr/share/live_magazine/always
%attr(640,root,app) /opt/dbspace/.livebox.db
%attr(640,root,app) /opt/dbspace/.livebox.db-journal

# End of a file
