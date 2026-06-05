#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# KVX-3 end-to-end: vendor KV Exec (ADR-0005) -> rados_aio_exec against an
# OSD-side object class, over a vfio-user loopback.
#
# Wires the librados-backed kvdev (kvdev_rados) to a KV namespace, allowlists
# op_id 1 -> "kvtest:echo" and op_id 2 -> "kvtest:upcase" (the binding format is
# "class:method"), then runs kv_host which Stores a value and issues KV Exec for
# both ops. The cls runs SERVER-SIDE on the vstart OSD: echo returns the input
# unchanged; upcase reads the stored object value and returns it uppercased.
#
# Requires: a vstart Ceph cluster UP at /mnt/ceph with pool 'kvpool', the
# libcls_kvtest.so object class loaded into the OSD (see cls/build_and_load.sh),
# and an SPDK build configured --with-rbd --with-vfio-user.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

: "${CEPH_CONF:=/mnt/ceph/build/ceph.conf}"
export CEPH_CONF
pool_name="${KV_RADOS_POOL:-kvpool}"
cluster_name="kvx_cluster"
kvdev_name="KvRados0"
nqn="nqn.2026-06.io.spdk:kv-rados-cnode0"
nsid=1

sock_dir=$(mktemp -d /tmp/kv_rados_exec.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_dir"

# Fail-fast OSD liveness precheck (KVX-3). KV Exec maps to an OSD-side
# rados_aio_exec; if the vstart OSD is DOWN or WEDGED the e2e would otherwise
# hang. Probe the pool with a hard, bounded timeout BEFORE bringing up the
# target so a dead cluster is reported in seconds with a clear message. We use
# the Ceph CLI from the same build dir as the conf.
OSD_PRECHECK_TIMEOUT_S="${OSD_PRECHECK_TIMEOUT_S:-15}"
rados_bin="$(dirname "$CEPH_CONF")/bin/rados"
[[ -x "$rados_bin" ]] || rados_bin="rados"
if ! timeout "$OSD_PRECHECK_TIMEOUT_S" "$rados_bin" -c "$CEPH_CONF" -p "$pool_name" ls >/dev/null 2>&1; then
	echo "kv_rados_exec: FAIL (OSD/pool '$pool_name' not reachable within ${OSD_PRECHECK_TIMEOUT_S}s; is the vstart OSD up?)"
	exit 1
fi
echo "OSD liveness precheck: pool '$pool_name' reachable"

cleanup() {
	if [[ -n ${nvmfpid:-} ]]; then
		killprocess $nvmfpid || true
	fi
	rm -rf "$sock_dir"
}
trap cleanup EXIT

# Build the host app if needed.
make -C "$testdir" >/dev/null

# Start the target.
$rootdir/build/bin/nvmf_tgt -r "$rpc_sock" -m 0x3 &
nvmfpid=$!
waitforlisten $nvmfpid "$rpc_sock"

# Wire up: VFIOUSER transport + rados cluster/kvdev + subsystem + KV ns + listener.
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_rados_register_cluster "$cluster_name" --user admin --config-file "$CEPH_CONF"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$pool_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKV002 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Allowlist (ADR-0005): op_id 1 -> kvtest:echo, op_id 2 -> kvtest:upcase,
# op_id 3 -> kvtest:fixedout (emits exactly N bytes, N = LE uint32 in the first
# 4 input bytes; drives the EXACT-FIT and OVER-LARGE BUFFER_TOO_SMALL checks).
# Binding format is "class:method"; the CLI splits each token on the FIRST ':'
# into op_id and binding, so "1:kvtest:echo" yields op_id=1 binding="kvtest:echo".
$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn" "$nsid" "1:kvtest:echo 2:kvtest:upcase 3:kvtest:fixedout"
get_json=$($rpc_py nvmf_ns_get_kv_exec_allowlist "$nqn" "$nsid")
echo "nvmf_ns_get_kv_exec_allowlist => $get_json"

# Run the host: Store + KV Exec(echo) + KV Exec(upcase), all against the librados
# backend. The cls executes on the OSD; the host asserts the returned bytes.
host_log="$sock_dir/kv_host.log"
"$testdir/kv_host" "$muser_dir" rados-exec 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]] && ! grep -q "PASS: KV Exec rados e2e (OSD-side kvtest cls ran)" "$host_log"; then
	echo "kv_rados_exec: FAIL (rados KV Exec e2e did not report PASS)"
	rc=1
fi

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
$rpc_py kvdev_rados_delete "$kvdev_name"
$rpc_py kvdev_rados_unregister_cluster "$cluster_name"

if [[ $rc -eq 0 ]]; then
	echo "kv_rados_exec: PASS"
else
	echo "kv_rados_exec: FAIL (rc=$rc)"
fi
exit $rc
