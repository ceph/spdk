# KV Exec control-plane sidecar (ADR-0005)

A standalone **gRPC sidecar** that exposes get/set of the **per-namespace KV Exec
allowlist** and applies it to a running SPDK NVMe-oF target via SPDK's existing
JSON-RPC interface.

This implements the control-plane half of [ADR-0005](../../docs/adr/0005-kv-exec-vendor-command-to-rados-cls.md):
the data path names an allowlisted operation by a small integer **op-ID**, and a
**privileged control plane** maps op-ID → (cls, method) binding per namespace.
This sidecar is that control plane.

## Why a Python sidecar (gRPC is NOT in the SPDK C build)

gRPC/protobuf are deliberately kept **out** of the SPDK C target core. The
sidecar is a separate process that reuses the JSON-RPC client SPDK already ships
(`python/spdk/rpc`). Nothing here is wired into the SPDK Makefile/meson build or
linked into the reactor app — it is self-contained in this directory. This avoids
adding grpc++/protobuf and a C++ server thread to the C reactor app (ADR-0005
"Why these choices").

## Layout

| File | Why |
|------|-----|
| `kv_exec_sidecar.proto` | gRPC service: `GetAllowlist` / `SetAllowlist(... repeated {op_id, binding})`, empty list clears. |
| `kv_exec_sidecar.py` | gRPC server; translates each RPC to `nvmf_ns_{get,set}_kv_exec_allowlist` JSON-RPC and maps errors to gRPC status codes. |
| `kv_exec_client.py` | Minimal CLI gRPC client for testing/ops. |
| `kv_exec_sidecar_pb2*.py` | Generated stubs (regenerate with the command below). |

## Generate stubs

```bash
pip install grpcio grpcio-tools          # if needed
python -m grpc_tools.protoc -I. \
    --python_out=. --grpc_python_out=. kv_exec_sidecar.proto
```

## Run

```bash
# SPDK target already running with a KV namespace, JSON-RPC on /var/tmp/spdk.sock
python kv_exec_sidecar.py \
    --socket /var/tmp/spdk.sock \
    --listen 127.0.0.1:50051

# In another shell:
python kv_exec_client.py set nqn.2016-06.io.spdk:cnode1 1 1 2:cls.method
python kv_exec_client.py get nqn.2016-06.io.spdk:cnode1 1
python kv_exec_client.py set nqn.2016-06.io.spdk:cnode1 1     # clear
```

## Error mapping

| JSON-RPC condition | gRPC status |
|--------------------|-------------|
| cannot reach SPDK socket | `UNAVAILABLE` |
| subsystem / namespace not found | `NOT_FOUND` |
| invalid params (bad nsid, etc.) | `INVALID_ARGUMENT` |
| any other JSON-RPC error | `INTERNAL` |
| client input fails validation | `INVALID_ARGUMENT` (never reaches target) |

## Auth / trust boundary (read this)

This sidecar is the **privileged control plane** for KV Exec. A caller of
`SetAllowlist` decides **which OSD-side object-class (`cls`) operations a KV
namespace may execute** — effectively granting server-side code execution on the
storage backend with OSD privileges (ADR-0005 §"Op-ID indirection"). Treat it
like a root-equivalent admin API:

- **Network isolation.** Bind only to a trusted, tenant-isolated interface
  (default `127.0.0.1:50051`). It must **never** be reachable by NVMe-oF
  initiators / tenants or the data plane. The op-ID→binding decision is the whole
  security boundary; if a tenant can call `SetAllowlist`, the indirection is moot.
- **Privileged socket.** The SPDK JSON-RPC unix socket is itself privileged. Run
  the sidecar where it can reach that socket and nowhere a tenant can.
- **Input validation (done here).** Before forwarding to the target the sidecar
  validates: `op_id` in `[0, 65535]`, no duplicate op-IDs, `binding` ≤ 255 bytes,
  printable and NUL-free. This keeps a hostile/buggy control client from smuggling
  malformed bindings into the target.
- **Transport security (deploy-time).** `add_insecure_port` is used for the
  in-cluster, isolated default. For any non-loopback deployment, front it with
  mTLS (`grpc.ssl_server_credentials`) and authenticate callers; this is an
  operational decision left to the deployer.
```
