#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# End-to-end Key-Value round-trip over vfio-user against a real Ceph cluster,
# backed by the librados kvdev (ADR-0002/0004).
#
# Requires a running Ceph cluster reachable via $CEPH_CONF/$CEPH_KEYRING and a
# pre-provisioned pool ($KV_POOL). It:
#   1. starts an nvmf target, registers a named rados cluster, creates a rados
#      kvdev (pool=$KV_POOL, namespace=$KV_NS), binds it to a KV namespace, and
#      exposes the subsystem over VFIOUSER;
#   2. runs the host in "store" mode (Store/Retrieve/Exist + List-not-supported);
#   3. confirms the object is present in rados via the rados CLI;
#   4. RESTARTS the target and runs the host in "verify" mode (Retrieve proves
#      the value survived the restart in rados, then Delete/Exist);
#   5. confirms the object is gone from rados after Delete.

set -e

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")

: "${CEPH_CONF:?set CEPH_CONF to the ceph.conf path}"
: "${CEPH_KEYRING:?set CEPH_KEYRING to the keyring path}"
: "${RADOS_BIN:=rados}"
KV_POOL="${KV_POOL:-kvpool}"
KV_NS="${KV_NS:-kvns}"
CEPH_USER="${CEPH_USER:-admin}"
# oid is the hex encoding of the key "kvkey01" (matches kvdev_rados_key_to_oid).
OID_HEX=$(printf '%s' "kvkey01" | od -An -tx1 | tr -d ' \n')

nqn="nqn.2026-06.io.spdk:kv-rados0"
cluster_name="ceph0"
kvdev_name="KvRados0"
sock_dir=$(mktemp -d /tmp/kv_rados_vfio.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_dir"

nvmfpid=""
cleanup() {
	[[ -n $nvmfpid ]] && kill "$nvmfpid" 2>/dev/null || true
	wait "$nvmfpid" 2>/dev/null || true
	rm -rf "$sock_dir"
}
trap cleanup EXIT

start_target() {
	"$rootdir/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x3 --iova-mode=va &
	nvmfpid=$!
	# wait for the RPC socket
	for _ in $(seq 1 40); do
		[[ -S $rpc_sock ]] && $rpc_py rpc_get_methods >/dev/null 2>&1 && return 0
		sleep 0.25
	done
	echo "target failed to come up"; return 1
}

wire_up() {
	$rpc_py nvmf_create_transport -t VFIOUSER
	$rpc_py kvdev_rados_register_cluster "$cluster_name" \
		--user "$CEPH_USER" --config-file "$CEPH_CONF" --key-file "$CEPH_KEYRING"
	$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$KV_POOL" --namespace "$KV_NS"
	$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKVR01 -a
	$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
	$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
}

echo "### oid for key 'kvkey01' = $OID_HEX (hex-encoded)"

# --- Phase 1: store ---
start_target
wire_up
echo "### Phase 1: host store mode"
"$testdir/kv_rados_host" "$muser_dir" store
echo "### rados object present after Store?"
"$RADOS_BIN" -c "$CEPH_CONF" -k "$CEPH_KEYRING" -p "$KV_POOL" -N "$KV_NS" stat "$OID_HEX"
echo "### object value via rados get:"
"$RADOS_BIN" -c "$CEPH_CONF" -k "$CEPH_KEYRING" -p "$KV_POOL" -N "$KV_NS" get "$OID_HEX" - | cat -v

# tear down the target (keep the rados objects)
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
kill "$nvmfpid"; wait "$nvmfpid" 2>/dev/null || true
nvmfpid=""

echo "### confirming object persists in rados while target is DOWN"
"$RADOS_BIN" -c "$CEPH_CONF" -k "$CEPH_KEYRING" -p "$KV_POOL" -N "$KV_NS" stat "$OID_HEX"

# --- Phase 2: restart + verify persistence, then delete ---
echo "### Phase 2: restart target, host verify mode"
start_target
wire_up
"$testdir/kv_rados_host" "$muser_dir" verify

echo "### rados object absent after Delete?"
if "$RADOS_BIN" -c "$CEPH_CONF" -k "$CEPH_KEYRING" -p "$KV_POOL" -N "$KV_NS" stat "$OID_HEX" 2>/dev/null; then
	echo "FAIL: object still present after Delete"
	exit 1
fi
echo "### object correctly absent after Delete"

echo "kv_rados_vfio_user: PASS"
