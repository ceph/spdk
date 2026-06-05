#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# End-to-end Key-Value round-trip over a vfio-user loopback.
#
# Starts an nvmf target, creates an in-memory kvdev, binds it to a KV namespace
# via the new nvmf_subsystem_add_kv_ns RPC, exposes the subsystem over the
# VFIOUSER transport, then runs the kv_host app which attaches over vfio-user
# (exercising the host PCIe attach + KV identify path) and does a KV Store then
# Retrieve, verifying the bytes round-trip.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

nqn="nqn.2026-06.io.spdk:kv-cnode0"
kvdev_name="KvMem0"
sock_dir=$(mktemp -d /tmp/kv_vfio_user.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_dir"

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

# Wire up: VFIOUSER transport, in-memory kvdev, subsystem + KV namespace + listener.
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_mem_create "$kvdev_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKV001 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Run the host: attach over vfio-user and do Store + Retrieve.
# The listener creates a socket at "$muser_dir/cntrl"; the host's vfio-user
# transport address is the directory that contains it.
"$testdir/kv_host" "$muser_dir"
rc=$?

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
$rpc_py kvdev_mem_delete "$kvdev_name"

if [[ $rc -eq 0 ]]; then
	echo "kv_vfio_user: PASS"
else
	echo "kv_vfio_user: FAIL (rc=$rc)"
fi
exit $rc
