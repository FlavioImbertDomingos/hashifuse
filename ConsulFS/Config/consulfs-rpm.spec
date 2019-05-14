﻿Name:           consulfs
Version:        0.1
Release:        1
Summary:        Hashicorp ConsulFS Fuse Agent
License:        GPL+
Source0:        consulfs
Requires(post): libcurl fuse jsoncpp
URL:            https://www.consul.io/

%define debug_package %{nil}

%description
FUSE filesystem for browsing and managing Hashicorp Consul KV and services.

%prep
%autosetup -c %{name}-%{version}

%build

%install

mkdir -p %{buildroot}%{_bindir}/
cp -p %{name} %{buildroot}%{_bindir}/

%clean
rm -rf %{buildroot}
rm -rf %{_builddir}/*

%files
%{_bindir}/%{name}

%changelog
