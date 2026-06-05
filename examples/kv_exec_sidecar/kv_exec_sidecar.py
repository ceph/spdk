#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation and contributors. All rights reserved.
#
"""KV Exec control-plane gRPC sidecar (ADR-0005).

A standalone process that exposes get/set of the per-namespace KV Exec
allowlist over gRPC and translates each call into the SPDK JSON-RPC methods
``nvmf_ns_{get,set}_kv_exec_allowlist`` against a target's local unix socket.

gRPC / protobuf are intentionally kept *out* of the SPDK C target: this is a
separate Python process that reuses the JSON-RPC client SPDK already ships.

TRUST BOUNDARY
--------------
This service is the privileged control plane for KV Exec. A caller of
SetAllowlist decides which OSD-side object-class (cls) operations a KV
namespace may execute -- effectively granting code execution on the storage
backend. Therefore:

  * Bind only to a trusted, tenant-isolated network/interface (default:
    localhost). Never expose it to NVMe-oF initiators / tenants.
  * The unix socket of the SPDK target is itself privileged; the sidecar must
    run where it can reach that socket and nowhere a tenant can.
  * All caller input (op_id range, binding string) is validated here before it
    is forwarded to the target.
"""

import argparse
import os
import socket
import sys
from concurrent import futures

# Make the SPDK python package importable. This file lives at
# <repo>/examples/kv_exec_sidecar/kv_exec_sidecar.py, so the package is at
# <repo>/python.
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(_REPO_ROOT, "python"))
# Generated stubs live alongside this file.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import grpc
except ImportError:
    sys.exit("error: grpcio is not installed. Run: pip install grpcio grpcio-tools")

try:
    import kv_exec_sidecar_pb2 as pb2
    import kv_exec_sidecar_pb2_grpc as pb2_grpc
except ImportError:
    sys.exit("error: gRPC stubs not found. Generate them with:\n"
             "  python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. "
             "kv_exec_sidecar.proto")

from spdk.rpc.client import JSONRPCClient, JSONRPCException

# Mirrors include/spdk/jsonrpc.h
SPDK_JSONRPC_ERROR_INVALID_PARAMS = -32602

# Validation bounds. The KV Exec opcode is a vendor I/O opcode; op-IDs are the
# small integers the data path carries. Keep them in a sane uint16 range and
# keep bindings short and printable so a hostile control client can't smuggle
# garbage into the target.
MAX_OP_ID = 0xFFFF
MAX_BINDING_LEN = 255


class JsonRpcError(Exception):
    """A JSON-RPC error response, carrying its numeric code and message."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _rpc_call(addr, timeout, method, params):
    """Issue one JSON-RPC call and return its result.

    Unlike JSONRPCClient.call(), this surfaces the structured error object so
    we can map the JSON-RPC error code to a gRPC status code.
    """
    try:
        client = JSONRPCClient(addr, timeout=timeout)
    except JSONRPCException as ex:
        raise JsonRpcError(None, str(ex))
    try:
        req_id = client.send(method, params)
        response = client.recv()
    finally:
        client.close()

    if "error" in response:
        err = response["error"]
        raise JsonRpcError(err.get("code"), err.get("message", "JSON-RPC error"))
    return response.get("result")


def _validate_entries(entries):
    """Validate gRPC AllowlistEntry list; return JSON-RPC allowlist params.

    Raises ValueError on bad input (mapped to INVALID_ARGUMENT by the caller).
    """
    seen = set()
    out = []
    for e in entries:
        op_id = e.op_id
        if op_id < 0 or op_id > MAX_OP_ID:
            raise ValueError("op_id %d out of range [0, %d]" % (op_id, MAX_OP_ID))
        if op_id in seen:
            raise ValueError("duplicate op_id %d" % op_id)
        seen.add(op_id)
        entry = {"op_id": op_id}
        binding = e.binding
        if binding:
            if len(binding) > MAX_BINDING_LEN:
                raise ValueError("binding for op_id %d exceeds %d bytes"
                                 % (op_id, MAX_BINDING_LEN))
            if not binding.isprintable() or "\x00" in binding:
                raise ValueError("binding for op_id %d is not printable" % op_id)
            entry["binding"] = binding
        out.append(entry)
    return out


def _validate_ns(ns, context):
    if not ns.nqn:
        context.abort(grpc.StatusCode.INVALID_ARGUMENT, "nqn is required")
    if ns.nsid == 0:
        context.abort(grpc.StatusCode.INVALID_ARGUMENT, "nsid is required and must be > 0")


def _ns_params(ns):
    params = {"nqn": ns.nqn, "nsid": ns.nsid}
    if ns.tgt_name:
        params["tgt_name"] = ns.tgt_name
    return params


def _result_to_entries(result):
    entries = []
    for item in result or []:
        entries.append(pb2.AllowlistEntry(op_id=item["op_id"],
                                          binding=item.get("binding", "")))
    return entries


def _map_jsonrpc_error(err, context):
    """Translate a JsonRpcError into a gRPC abort with a sensible status."""
    if err.code is None:
        # Could not even reach / talk to the target socket.
        context.abort(grpc.StatusCode.UNAVAILABLE,
                      "cannot reach SPDK target: %s" % err.message)
    msg = (err.message or "").lower()
    if err.code == SPDK_JSONRPC_ERROR_INVALID_PARAMS:
        # The target collapses "subsystem/ns not found" and bad params into
        # INVALID_PARAMS. Distinguish on the message where we can.
        if "not found" in msg or "no such" in msg or "unable to find" in msg:
            context.abort(grpc.StatusCode.NOT_FOUND, err.message)
        context.abort(grpc.StatusCode.INVALID_ARGUMENT, err.message)
    context.abort(grpc.StatusCode.INTERNAL,
                  "JSON-RPC error %s: %s" % (err.code, err.message))


class KvExecControlServicer(pb2_grpc.KvExecControlServicer):
    def __init__(self, addr, timeout):
        self._addr = addr
        self._timeout = timeout

    def GetAllowlist(self, request, context):
        _validate_ns(request.ns, context)
        try:
            result = _rpc_call(self._addr, self._timeout,
                               "nvmf_ns_get_kv_exec_allowlist",
                               _ns_params(request.ns))
        except JsonRpcError as err:
            _map_jsonrpc_error(err, context)
        return pb2.GetAllowlistResponse(allowlist=_result_to_entries(result))

    def SetAllowlist(self, request, context):
        _validate_ns(request.ns, context)
        try:
            allowlist = _validate_entries(request.allowlist)
        except ValueError as ex:
            context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(ex))

        params = _ns_params(request.ns)
        params["allowlist"] = allowlist
        try:
            _rpc_call(self._addr, self._timeout,
                      "nvmf_ns_set_kv_exec_allowlist", params)
            # Read back so the response reflects what the target actually stored.
            result = _rpc_call(self._addr, self._timeout,
                               "nvmf_ns_get_kv_exec_allowlist",
                               _ns_params(request.ns))
        except JsonRpcError as err:
            _map_jsonrpc_error(err, context)
        return pb2.SetAllowlistResponse(allowlist=_result_to_entries(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-s", "--socket", default="/var/tmp/spdk.sock",
                        help="SPDK target JSON-RPC unix socket (default: /var/tmp/spdk.sock)")
    parser.add_argument("-l", "--listen", default="127.0.0.1:50051",
                        help="gRPC listen address (default: 127.0.0.1:50051). "
                             "Keep this on a tenant-isolated interface.")
    parser.add_argument("-t", "--timeout", type=float, default=60.0,
                        help="JSON-RPC call timeout in seconds (default: 60)")
    args = parser.parse_args()

    if not os.path.exists(args.socket):
        print("warning: SPDK socket %s does not exist yet; "
              "calls will fail until the target is up" % args.socket,
              file=sys.stderr)

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    pb2_grpc.add_KvExecControlServicer_to_server(
        KvExecControlServicer(args.socket, args.timeout), server)
    server.add_insecure_port(args.listen)
    server.start()
    print("kv_exec_sidecar listening on %s -> SPDK socket %s"
          % (args.listen, args.socket), file=sys.stderr)
    server.wait_for_termination()


if __name__ == "__main__":
    main()
