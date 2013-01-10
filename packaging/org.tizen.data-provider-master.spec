Name: org.tizen.data-provider-master
Summary: Master data provider
Version: 0.13.31
Release: 1
Group: main/app
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
Manage the slave data provider and communicate with client applications.

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix}
CFLAGS="${CFLAGS} -Wall -Winline -Werror" LDFLAGS="${LDFLAGS}" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datarootdir}/license

%pre

# Executing the stop script for stopping the service of installed provider (old version)
if [ -x %{_sysconfdir}/rc.d/init.d/data-provider-master ]; then
	%{_sysconfdir}/rc.d/init.d/data-provider-master stop
fi

%post

mkdir -p /opt/usr/share/live_magazine
chown 5000:5000 /opt/usr/share/live_magazine
if [ -f %{_libdir}/rpm-plugins/msm.so ]; then
	echo "Update smack for CONTENT SHARING FOLDER"
	chsmack -a "_" /opt/usr/share/live_magazine
	chsmack -t /opt/usr/share/live_magazine
fi

# According to this transmute attribute, below log, reader folder will be set as same label

mkdir -p /opt/usr/share/live_magazine/log
chown 5000:5000 /opt/usr/share/live_magazine/log

mkdir -p /opt/usr/share/live_magazine/reader
chown 5000:5000 /opt/usr/share/live_magazine/reader

mkdir -p /opt/usr/share/live_magazine/always
chown 5000:5000 /opt/usr/share/live_magazine/always

# End of a list of affected folder by the transmute attribute

if [ ! -f "/opt/dbspace/livebox.db" ]; then
	echo "Create a new livebox DB"
	touch /opt/dbspace/.livebox.db
	chown 0:5000 /opt/dbspace/.livebox.db
	chmod 640 /opt/dbspace/.livebox.db
	if [ -f %{_libdir}/rpm-plugins/msm.so ]; then
		echo "Update smack for DB"
		chsmack -a "data-provider-master::db" /opt/dbspace/.livebox.db
	fi
fi

if [ ! -f "/opt/dbspace/livebox.db-journal" ]; then
	echo "Create a new livebox DB - journal file"
	touch /opt/dbspace/.livebox.db-journal
	chown 0:5000 /opt/dbspace/.livebox.db-journal
	chmod 640 /opt/dbspace/.livebox.db-journal
	if [ -f %{_libdir}/rpm-plugins/msm.so ]; then
		echo "Update smack for DB(journal)"
		chsmack -a "data-provider-master::db" /opt/dbspace/.livebox.db-journal
	fi
fi

mkdir -p %{_sysconfdir}/rc.d/rc3.d
ln -sf %{_sysconfdir}/rc.d/init.d/data-provider-master %{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
if [ -f %{_libdir}/rpm-plugins/msm.so ]; then
	echo "Update smack for INITD - booting script"
	chsmack -a "_" %{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
	chsmack -e "_" %{_sysconfdir}/rc.d/rc3.d/S99data-provider-master
fi

mkdir -p %{_libdir}/systemd/user/tizen-middleware.target.wants
ln -sf %{_libdir}/systemd/user/data-provider-master.service %{_libdir}/systemd/user/tizen-middleware.target.wants/data-provider-master.service
if [ -f %{_libdir}/rpm-plugins/msm.so ]; then
	echo "Update smack for SYSTEMD - service file"
	chsmack -a "_" %{_libdir}/systemd/user/tizen-middleware.target.wants/data-provider-master.service
fi

echo "Successfully installed. Please start a daemon again manually"
echo "%{_sysconfdir}/init.d/data-provider-master start"

%files -n org.tizen.data-provider-master
%manifest org.tizen.data-provider-master.manifest
%defattr(-,root,root,-)
%{_sysconfdir}/rc.d/init.d/data-provider-master
%{_bindir}/data-provider-master
%{_bindir}/liveinfo
%{_prefix}/etc/package-manager/parserlib/*
%{_datarootdir}/data-provider-master/*
%{_libdir}/systemd/user/data-provider-master.service
%{_datarootdir}/license/*

# End of a file
