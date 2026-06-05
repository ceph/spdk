#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Drive the SPDK NVMe-KV fio ioengine against the librados (Ceph) kvdev over
# vfio-user. Identical to test/nvmf/kv/kv_fio_verify.sh except the KV namespace
# is backed by a rados kvdev (pool=$KV_POOL, namespace=$KV_NS) instead of the
# in-memory module -- the fio plugin is unaware of the backend. fio's
# randwrite+crc32c verify exercises Store-then-Retrieve correctness, and the
# written objects land in rados (check with: rados -p $KV_POOL -N $KV_NS ls).
#
# Requires a running Ceph cluster reachable via $CEPH_CONF/$CEPH_KEYRING with a
# pre-provisioned pool ($KV_POOL), and an SPDK build with --with-rbd,
# --with-vfio-user and --with-fio=<fio-src>.

set -e

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"

: "${CEPH_CONF:?set CEPH_CONF to the ceph.conf path}"
: "${CEPH_KEYRING:?set CEPH_KEYRING to the keyring path}"
KV_POOL="${KV_POOL:-kvpool}"
KV_NS="${KV_NS:-kvns}"
CEPH_USER="${CEPH_USER:-admin}"

nqn="nqn.2026-06.io.spdk:kv-rados-fio0"
cluster_name="ceph0"
kvdev_name="KvRadosFio0"
sock_dir=$(mktemp -d /tmp/kv_rados_fio.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"
fio_plugin="$rootdir/build/fio/spdk_nvme"
fio_job="$rootdir/test/nvmf/kv/kv_verify.fio"
fio_bin="$CONFIG_FIO_SOURCE_DIR/fio"
[[ -x $fio_bin ]] || fio_bin=$(type -P fio || true)

mkdir -p "$muser_dir"
cleanup() {
	[[ -n ${nvmfpid:-} ]] && killprocess "$nvmfpid" || true
	rm -rf "$sock_dir"
}
trap cleanup EXIT

if [[ ! -e $fio_plugin ]]; then
	echo "kv_rados_fio: SKIP (SPDK fio plugin not built at $fio_plugin; reconfigure with --with-fio=<fio-src>)"
	exit 0
fi
if [[ -z $fio_bin || ! -x $fio_bin ]]; then
	echo "kv_rados_fio: FAIL (no fio binary; expected \$CONFIG_FIO_SOURCE_DIR/fio at '$CONFIG_FIO_SOURCE_DIR/fio')"
	exit 1
fi

# Start the target and build a rados-backed KV namespace.
"$rootdir/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x3 &
nvmfpid=$!
waitforlisten "$nvmfpid" "$rpc_sock"

$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_rados_register_cluster "$cluster_name" \
	--user "$CEPH_USER" --config-file "$CEPH_CONF" --key-file "$CEPH_KEYRING"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$KV_POOL" --namespace "$KV_NS"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKVRFIO1 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Run fio: randwrite then read-verify each object (Store -> Retrieve over rados).
escaped_nqn=${nqn//:/\\:}
fio_log="$sock_dir/fio.log"
LD_PRELOAD="$fio_plugin" "$fio_bin" "$fio_job" \
	--filename="trtype=VFIOUSER traddr=${muser_dir} subnqn=${escaped_nqn} ns=1" \
	2>&1 | tee "$fio_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]]; then
	verify_re='verify[ _]failed|bad magic|verify bytes|data mismatch'
	verify_re+='|io_u (verify|error)|completed with error|err= *[1-9]'
	if grep -qiE "$verify_re" "$fio_log"; then
		echo "kv_rados_fio: FAIL (fio reported verify/io errors)"
		rc=1
	fi
fi

[[ $rc -eq 0 ]] && echo "kv_rados_fio: PASS (fio Store/Retrieve+verify over librados)" || echo "kv_rados_fio: FAIL"
exit $rc
