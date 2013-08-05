Name: data-provider-master
Summary: Master service provider for liveboxes.
Version: 0.25.2
Release: 1
Group: HomeTF/Livebox
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
BuildRequires: pkgconfig(libsmack)
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
BuildRequires: pkgconfig(libtbm)
BuildRequires: pkgconfig(xfixes)
BuildRequires: pkgconfig(dri2proto)
BuildRequires: pkgconfig(xext)
BuildRequires: pkgconfig(xdamage)
BuildRequires: pkgconfig(pkgmgr)
BuildRequires: pkgconfig(livebox-service)
BuildRequires: pkgconfig(notification)
BuildRequires: pkgconfig(badge)
BuildRequires: pkgconfig(shortcut)
BuildRequires: pkgconfig(security-server)
Requires(post): sys-assert
Requires(post): dbus

%description
Manage the 2nd stage livebox service provider and communicate with the viewer application.
Keep trace on the life-cycle of the livebox and status of the service providers, viewer applications.

%prep
%setup -q

%build
%if 0%{?tizen_build_binary_release_type_eng}
export CFLAGS="${CFLAGS} -DTIZEN_ENGINEER_MODE"
export CXXFLAGS="${CXXFLAGS} -DTIZEN_ENGINEER_MODE"
export FFLAGS="${FFLAGS} -DTIZEN_ENGINEER_MODE"
%endif

%if 0%{?sec_product_feature_livebox_shm}
	cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix} -DPRODUCT=baltic
%else
	cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix} -DPRODUCT=private
%endif

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
#mkdir -p %{buildroot}/%{_sysconfdir}/rc.d/rc3.d
mkdir -p %{buildroot}/%{_libdir}/systemd/user/tizen-middleware.target.wants
touch %{buildroot}/opt/dbspace/.livebox.db
touch %{buildroot}/opt/dbspace/.livebox.db-journal
#ln -sf %{_sysconfdir}/rc.d/init.d/data-provider-master %{buildroot}/%{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
ln -sf %{_libdir}/systemd/user/data-provider-master.service %{buildroot}/%{_libdir}/systemd/user/tizen-middleware.target.wants/data-provider-master.service
if [ ! -s %{buildroot}/opt/dbspace/.livebox.db ]; then
echo "LiveBox DB file is not exists, initiate it"
sqlite3 %{buildroot}/opt/dbspace/.livebox.db <<EOF
CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, preview TEXT, touch_effect INTEGER, need_frame INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE client (pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, auto_launch TEXT, pd_size TEXT, content TEXT DEFAULT 'default', nodisplay INTEGER, setup TEXT, mouse_event INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, pkgid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE groupmap (option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT COLLATE NOCASE, name TEXT, icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, FOREIGN KEY(option_id) REFERENCES groupmap(option_id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, uiapp TEXT, prime INTEGER );
CREATE TABLE provider ( pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, abi TEXT, secured INTEGER, box_type INTEGER, box_src TEXT, box_group TEXT, pd_type INTEGER, pd_src TEXT, pd_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
EOF
fi

%pre
# Executing the stop script for stopping the service of installed provider (old version)
if [ -x %{_sysconfdir}/rc.d/init.d/data-provider-master ]; then
	%{_sysconfdir}/rc.d/init.d/data-provider-master stop
fi

%post
chown 5000:5000 /opt/usr/share/live_magazine
chmod 750 /opt/usr/share/live_magazine
chown 5000:5000 /opt/usr/share/live_magazine/log
chmod 750 /opt/usr/share/live_magazine/log
chown 5000:5000 /opt/usr/share/live_magazine/reader
chmod 750 /opt/usr/share/live_magazine/reader
chown 5000:5000 /opt/usr/share/live_magazine/always
chmod 750 /opt/usr/share/live_magazine/always
chown 0:5000 /opt/dbspace/.livebox.db
chmod 640 /opt/dbspace/.livebox.db
chown 0:5000 /opt/dbspace/.livebox.db-journal
chmod 640 /opt/dbspace/.livebox.db-journal
vconftool set -t bool "memory/data-provider-master/started" 0 -i -u 5000 -f -s system::vconf_system
vconftool set -t string "db/data-provider-master/serveraddr" "/opt/usr/share/live_magazine/.client.socket" -i -u 5000 -f -s system::vconf_system
echo "Successfully installed. Please start a daemon again manually"
echo "%{_sysconfdir}/init.d/data-provider-master start"

%files -n data-provider-master
%manifest data-provider-master.manifest
%defattr(-,root,root,-)
%{_sysconfdir}/rc.d/init.d/data-provider-master
#%{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
%{_bindir}/data-provider-master
%{_bindir}/liveinfo
%{_prefix}/etc/package-manager/parserlib/*
%{_datarootdir}/data-provider-master/*
%{_libdir}/systemd/user/data-provider-master.service
%{_libdir}/systemd/user/tizen-middleware.target.wants/data-provider-master.service
%{_datarootdir}/license/*
/opt/usr/share/live_magazine/*
/opt/dbspace/.livebox.db
/opt/dbspace/.livebox.db-journal
/opt/etc/smack/accesses.d/*

# End of a file
