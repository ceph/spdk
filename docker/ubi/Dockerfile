# syntax = docker/dockerfile:1.4
ARG OS_RELEASE=8
FROM registry.access.redhat.com/ubi$OS_RELEASE/ubi:latest as prebase
ARG OS_RELEASE

# Speed up dnf
COPY <<EOF /etc/dnf/dnf.conf
[main]
gpgcheck=1
installonly_limit=3
clean_requirements_on_remove=True
skip_if_unavailable=False
best=False
fastestmirror=True
max_parallel_downloads=10
tsflags=nodocs
EOF

#------------------------------------------------------------------------------
FROM prebase as base_release_8
ARG CENTOS_PACKAGE_VERSION="8-6"
ARG CENTOS_MIRROR="mirror.centos.org/centos"

#------------------------------------------------------------------------------
FROM prebase as base_release_9
ARG CENTOS_PACKAGE_VERSION="9.0-20"
ARG CENTOS_MIRROR="mirror.stream.centos.org"

#------------------------------------------------------------------------------
FROM base_release_$OS_RELEASE as base

ARG CENTOS_URL="http://$CENTOS_MIRROR/$OS_RELEASE-stream/BaseOS/x86_64/os/Packages"
ARG CENTOS_GPG_PACKAGE="$CENTOS_URL/centos-gpg-keys-$CENTOS_PACKAGE_VERSION.el$OS_RELEASE.noarch.rpm"
ARG CENTOS_REPO_PACKAGE="$CENTOS_URL/centos-stream-repos-$CENTOS_PACKAGE_VERSION.el$OS_RELEASE.noarch.rpm"

ARG CEPH_VERSION="17.2.5"


COPY <<EOF /etc/yum.repos.d/ceph.repo 
[Ceph]
name=Ceph packages for \$basearch
baseurl=http://download.ceph.com/rpm-$CEPH_VERSION/el\$releasever/\$basearch
enabled=1
gpgcheck=1
type=rpm-md
gpgkey=https://download.ceph.com/keys/release.asc

[Ceph-noarch]
name=Ceph noarch packages
baseurl=http://download.ceph.com/rpm-$CEPH_VERSION/el\$releasever/noarch
enabled=1
gpgcheck=1
type=rpm-md
gpgkey=https://download.ceph.com/keys/release.asc

[ceph-source]
name=Ceph source packages
baseurl=http://download.ceph.com/rpm-$CEPH_VERSION/el\$releasever/SRPMS
enabled=1
gpgcheck=1
type=rpm-md
gpgkey=https://download.ceph.com/keys/release.asc
EOF

RUN \
    --mount=type=cache,target=/var/cache/dnf \
    --mount=type=cache,target=/var/lib/dnf \
    dnf remove -y dnf-plugin-subscription-manager \
    && dnf install -y \
        $CENTOS_GPG_PACKAGE \
        $CENTOS_REPO_PACKAGE

#------------------------------------------------------------------------------
FROM base as build

ARG SPDK_VERSION

ARG RPM_RELEASE
ARG MAKEFLAGS
ARG PKGDEP_ARGS="--rbd"
ARG CONFIGURE_ARGS="\
  --with-rbd \
  --disable-tests \
  --disable-unit-tests \
  --disable-examples \
"

WORKDIR /src
COPY . .

RUN \
    --mount=type=cache,target=/var/cache/dnf \
    --mount=type=cache,target=/var/lib/dnf \
    --mount=type=cache,target=/root/.cache/pip \
    dnf install -y \
        rpm-build \
        git \
    && scripts/pkgdep.sh $PKGDEP_ARGS

RUN DEPS="no" rpmbuild/rpm.sh $CONFIGURE_ARGS

#------------------------------------------------------------------------------
FROM base as spdk

# @TODO: Add LABELs

RUN \
    --mount=type=bind,from=build,source=/root/rpmbuild/rpm,target=/rpm \
    --mount=type=cache,target=/var/cache/dnf \
    --mount=type=cache,target=/var/lib/dnf \
    dnf install -y /rpm/$(uname -m)/*.rpm
