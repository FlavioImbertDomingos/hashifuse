Name:           k8sfs
Version:        0.1
Release:        1
Summary:        Hashicorp Kubernetes Fuse Agent
License:        GPL+
Source0:        https://github.com/jboero/hashifuse/archive/master.zip
Requires(post): libcurl fuse jsoncpp
URL:            https://www.consul.io/

%define debug_package %{nil}

%description
FUSE filesystem for browsing and managing Kubernetes resources as files.

%prep
%autosetup -c %{name}-%{version}

%build
cd hashifuse-master/K8sFS
make

%install

mkdir -p %{buildroot}%{_bindir}/
cp -p hashifuse-master/K8sFS/%{name} %{buildroot}%{_bindir}/

%clean
rm -rf %{buildroot}
rm -rf %{_builddir}/*

%files
%{_bindir}/%{name}

%changelog