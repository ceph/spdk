#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# End-to-end test of the SPDK NVMe fio plugin against a Key-Value namespace
# over a vfio-user loopback.
#
# Starts an nvmf target, creates an in-memory kvdev, binds it to a KV namespace
# (nvmf_subsystem_add_kv_ns), exposes the subsystem over the VFIOUSER transport,
# then runs fio with the SPDK ioengine against that namespace. The job does a
# random write followed by a read-verify: the plugin maps write->KV Store and
# read->KV Retrieve, deriving each object's key from the io_u offset, so the
# verify pass re-reads every written object and checks its CRC.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

nqn="nqn.2026-06.io.spdk:kv-fio-cnode0"
kvdev_name="KvMemFio0"
sock_dir=$(mktemp -d /tmp/kv_fio_verify.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

# The SPDK fio plugin lives here once `make` has built it.
fio_plugin="$rootdir/build/fio/spdk_nvme"
fio_job="$testdir/kv_verify.fio"

mkdir -p "$muser_dir"

cleanup() {
	if [[ -n ${nvmfpid:-} ]]; then
		killprocess $nvmfpid || true
	fi
	rm -rf "$sock_dir"
}
trap cleanup EXIT

if [[ ! -e $fio_plugin ]]; then
	echo "kv_fio_verify: SKIP (SPDK fio plugin not built at $fio_plugin;"
	echo "  reconfigure with ./configure --with-fio=<fio-src> and run make)"
	exit 0
fi

# Start the target.
$rootdir/build/bin/nvmf_tgt -r "$rpc_sock" -m 0x3 &
nvmfpid=$!
waitforlisten $nvmfpid "$rpc_sock"

# Wire up: VFIOUSER transport, in-memory kvdev, subsystem + KV namespace + listener.
# Size the kvdev so its advertised keyspace comfortably covers the fio job
# (size=4m / bs=4k = 1024 objects of 4096 bytes).
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_mem_create "$kvdev_name" --max-value-len 4096 --max-num-keys 4096
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKVFIO1 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Run fio with the SPDK ioengine against the KV namespace over vfio-user.
# The listener creates a socket at "$muser_dir/cntrl"; the vfio-user transport
# address is the directory that contains it.
#
# We attach directly to the subsystem (subnqn=$nqn) rather than via discovery.
# fio uses ':' as its filename-list separator, and an NVMe NQN contains ':',
# so the colons in the filename must be escaped as '\:' for fio to treat the
# whole transport URI as a single filename.
escaped_nqn=${nqn//:/\\:}
fio_log="$sock_dir/fio.log"
LD_PRELOAD="$fio_plugin" fio "$fio_job" \
	--filename="trtype=VFIOUSER traddr=${muser_dir} subnqn=${escaped_nqn} ns=1" \
	2>&1 | tee "$fio_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]]; then
	# fio returns 0 on success; verify=crc32c makes a failed verify fatal, so a
	# clean exit means every Store round-tripped through Retrieve.
	if grep -qiE "verify failed|bad magic|verify bytes|err= *[1-9]" "$fio_log"; then
		echo "kv_fio_verify: FAIL (fio reported verify/io errors)"
		rc=1
	fi
fi

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
$rpc_py kvdev_mem_delete "$kvdev_name"

if [[ $rc -eq 0 ]]; then
	echo "kv_fio_verify: PASS"
else
	echo "kv_fio_verify: FAIL (rc=$rc)"
fi
exit $rc
