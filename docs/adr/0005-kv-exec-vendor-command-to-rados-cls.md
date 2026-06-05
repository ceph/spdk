# Vendor "KV Exec" command → RADOS object-class (cls) execution

We add a **vendor-specific** NVMe KV command, **KV Exec**, that runs an allowlisted
RADOS **object-class method** (`cls`) server-side on the OSD that owns the object
backing a key — turning the KV namespace into a near-data-compute / computational-
storage surface. This is a deliberate non-standard extension, in the same spirit as the
vendor TTL ([0003](0003-vendor-ttl-extension-to-kv-store.md)).

## Design

- **Command**: a vendor I/O opcode (0x80–0xFF) in the KV command set — *not* a standard
  KV verb (none means "execute") and *not* the separate NVMe Computational Programs
  command set (far heavier, awkward fit). The command carries the key (CDW2/3/14/15), an
  input blob (PRP/SGL data; CDW10 = input length), and an **op-ID**; it returns an output
  blob to the host. A new `exec` op is added to the kvdev vtable.
- **Backend mapping**: the librados module maps KV Exec to `rados_aio_exec(ioctx, oid,
  completion, cls, method, in, in_len, out, out_len)`. The in-memory module stubs it (or
  supports a couple of built-ins) for testing.
- **Op-ID indirection (security)**: the data-plane command names the operation by a
  small integer **op-ID**, never by class/method strings. A control plane maps
  op-ID → (class, method, arg policy). Arbitrary `cls`/`method` strings therefore never
  cross the NVMe data path, so the **allowlist is enforced by construction** and the hot
  path does no string matching. An unknown/absent op-ID is rejected.
- **Control plane**: an **external gRPC sidecar** exposes get/set of the allowlist
  (op-ID → cls/method/policy) and applies it to the SPDK target via SPDK's existing
  JSON-RPC. gRPC (C++/protobuf) is kept out of the SPDK target core; control plane stays
  privileged and separate from the data plane.
- **Scope: per-namespace.** The allowlist (and therefore the op-ID → cls/method table) is
  scoped to a single KV namespace, not global. This matches the tenancy model — each KV
  namespace is its own rados namespace ([0004](0004-subsystem-to-pool-namespace-to-rados-namespace.md)),
  so a namespace's allowlist governs exactly the cls operations permitted on its own
  objects. The same op-ID may map to different (class, method) bindings in different
  namespaces, and the sidecar's get/set is keyed by (subsystem, namespace).

## Why these choices

- A vendor opcode is the smallest thing that fits and mirrors the TTL precedent.
- Op-ID indirection makes `rados_exec`'s broad power (it runs registered OSD-side code
  with OSD privileges) safe to expose: the trust decision lives in the control plane, not
  in initiator-supplied strings.
- A sidecar avoids linking grpc++/protobuf and a C++ server thread into the C reactor app.

## Scope / dependency

- This is a **new epic gated behind the librados kvdev slice** ([spdk-j8s] /
  [0002](0002-rados-object-per-kv-pair.md)) — `rados_exec` is rados-specific. The opcode,
  host API, kvdev `exec` vtable op, and an in-memory built-in can land earlier; the
  `rados_aio_exec` mapping and the allowlist/sidecar follow the rados backend.
- Considered and rejected: NVMe Computational Programs command set (too large for "call a
  named cls method"); overloading Store/Retrieve (conflates data with RPC, not atomic);
  class/method strings on the data path (attacker-controlled strings, per-IO matching).
