#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation and contributors. All rights reserved.
#
"""Minimal gRPC client for the KV Exec control-plane sidecar (ADR-0005).

Examples:
  # Set allowlist: op 1 (no binding), op 2 bound to cls.method
  kv_exec_client.py set nqn.2016-06.io.spdk:cnode1 1 1 2:cls.method

  # Read it back
  kv_exec_client.py get nqn.2016-06.io.spdk:cnode1 1

  # Clear (set with no entries)
  kv_exec_client.py set nqn.2016-06.io.spdk:cnode1 1
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import grpc
import kv_exec_sidecar_pb2 as pb2
import kv_exec_sidecar_pb2_grpc as pb2_grpc


def _parse_entry(spec):
    # "op_id" or "op_id:binding"
    parts = spec.split(":", 1)
    op_id = int(parts[0])
    binding = parts[1] if len(parts) > 1 else ""
    return pb2.AllowlistEntry(op_id=op_id, binding=binding)


def _print(entries):
    if not entries:
        print("(empty allowlist)")
    for e in entries:
        if e.binding:
            print("op_id=%d binding=%s" % (e.op_id, e.binding))
        else:
            print("op_id=%d" % e.op_id)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default="127.0.0.1:50051", help="sidecar gRPC address")
    ap.add_argument("--tgt-name", default="", help="SPDK NVMe-oF target name (optional)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("get")
    g.add_argument("nqn")
    g.add_argument("nsid", type=int)

    s = sub.add_parser("set")
    s.add_argument("nqn")
    s.add_argument("nsid", type=int)
    s.add_argument("entries", nargs="*", help="op_id or op_id:binding; none = clear")

    args = ap.parse_args()
    ns = pb2.NamespaceRef(nqn=args.nqn, nsid=args.nsid, tgt_name=args.tgt_name)

    with grpc.insecure_channel(args.target) as channel:
        stub = pb2_grpc.KvExecControlStub(channel)
        try:
            if args.cmd == "get":
                resp = stub.GetAllowlist(pb2.GetAllowlistRequest(ns=ns))
                _print(resp.allowlist)
            else:
                entries = [_parse_entry(e) for e in args.entries]
                resp = stub.SetAllowlist(pb2.SetAllowlistRequest(ns=ns, allowlist=entries))
                _print(resp.allowlist)
        except grpc.RpcError as e:
            print("gRPC error: %s: %s" % (e.code(), e.details()), file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
