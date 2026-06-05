#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation and contributors. All rights reserved.
#
# Standalone helper for the KV Exec control-plane sidecar (ADR-0005).
#
# NOTE: this Makefile is intentionally NOT wired into the SPDK build. The
# sidecar is a separate Python process; gRPC/protobuf must stay out of the
# SPDK C target core. This only regenerates the gRPC stubs from the .proto.

PYTHON ?= python3
PROTO  := kv_exec_sidecar.proto
STUBS  := kv_exec_sidecar_pb2.py kv_exec_sidecar_pb2_grpc.py

.PHONY: proto clean

proto: $(STUBS)

$(STUBS): $(PROTO)
	$(PYTHON) -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. $(PROTO)

clean:
	rm -f $(STUBS)
	rm -rf __pycache__
