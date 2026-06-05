#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Build the THROWAWAY KVX-3 test object class (cls_kvtest) against the Ceph tree
# and load it into the running vstart OSD so KV Exec -> rados_aio_exec can invoke
# it. NOT production tooling (ADR-0005 keeps real cls authoring to a later slice).
#
# Env (defaults target the in-repo vstart at /mnt/ceph):
#   CEPH_SRC   Ceph source tree (default /mnt/ceph/src)
#   CEPH_BUILD Ceph build dir   (default /mnt/ceph/build)
#
# The vstart ceph.conf already sets 'osd class load list = *' and
# 'osd class default list = *', so dropping the .so into the OSD class dir
# (osd class dir = $CEPH_BUILD/lib) is enough; the OSD loads the class lazily on
# first use, so no OSD restart is required.

set -eu

here=$(readlink -f "$(dirname "$0")")
CEPH_SRC="${CEPH_SRC:-/mnt/ceph/src}"
CEPH_BUILD="${CEPH_BUILD:-/mnt/ceph/build}"

out="$here/libcls_kvtest.so"

g++ -std=c++20 -fPIC -shared -o "$out" "$here/cls_kvtest.cc" \
	-I"$CEPH_SRC" -I"$CEPH_SRC/include" -I"$CEPH_BUILD/include" \
	-I"$CEPH_BUILD/src/include" -I"$CEPH_BUILD/boost/include" \
	-I"$CEPH_SRC/fmt/include" -DHAVE_CONFIG_H -D__CEPH__

echo "built $out"

# Install into the OSD class dir using the soname convention other cls libs use.
dst="$CEPH_BUILD/lib"
cp "$out" "$dst/libcls_kvtest.so.1.0.0"
ln -sf libcls_kvtest.so.1.0.0 "$dst/libcls_kvtest.so.1"
ln -sf libcls_kvtest.so.1 "$dst/libcls_kvtest.so"
echo "installed into $dst (loaded lazily by the OSD on first KV Exec)"
