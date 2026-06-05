# SPDK NVMe Key-Value (KV)

Glossary for the in-progress work to add NVMe Key-Value Command Set support to SPDK:
a target-side implementation of the KV commands, a swappable backend, an fio engine,
and (later) a Ceph/librados-backed store. The host-side (initiator) KV driver already
exists in `lib/nvme/nvme_kv.c`.

## Language

**KV namespace**:
An NVMf namespace that advertises the Key-Value Command Set (CSI = KV) instead of the
block NVM command set. It is backed by a [kvdev], not a `bdev`.
_Avoid_: KV bdev, KV volume.

**kvdev**:
The KV backend abstraction — a new SPDK library that is a sibling to `bdev`, with its own
module registration and descriptors. A kvdev services KV operations (store/retrieve/delete/
exist/list) against some store. Concrete modules implement it (e.g. an in-memory module and
a librados module).
_Avoid_: KV bdev, KV backend (when a precise term is needed), kv_store.

**KV operation**:
One of the Key-Value Command Set verbs serviced by a kvdev: Store, Retrieve, Delete, Exist,
List. The canonical verb names match the spec; do not rename them (e.g. not "put/get").
_Avoid_: put, get, set (these are not the spec's verbs).

**Key**:
A binary identifier, 1–16 bytes, naming a value within a KV namespace.
_Avoid_: object name, oid (those are Ceph-side concepts).

**Value**:
The variable-length payload stored under a Key.
_Avoid_: object, blob.

## Ceph / librados mapping

**Pool**:
A rados pool. Each NVMf [subsystem] that uses the librados backend maps to one
distinct pool.
_Avoid_: rados pool (redundant), bucket.

**Rados namespace**:
A namespace *within* a rados pool (set via the ioctx). Each KV [KV namespace] maps to
one rados namespace inside its subsystem's [Pool]. Distinct from an NVMe namespace —
always say "rados namespace" when you mean the Ceph one.
_Avoid_: pool namespace.

**Oid**:
The rados object name a [Key] is encoded into. With object-per-KV, one oid holds one
[Value] as its object data.
_Avoid_: object key, rados key.

## Extensions

**TTL**:
A vendor-specific, non-standard time-to-live attached to a Store. It is *persisted*
(as a rados xattr on the [Oid], or alongside the [Value] in the in-memory module) but
**not enforced** — an expired [Key] is still retrievable. Always note "store-only" when
describing current behavior, to distinguish it from a future enforced expiry.
_Avoid_: expiry, expiration (those imply enforcement we don't yet do).

**KV Exec**:
A vendor-specific KV command (vendor I/O opcode) that runs an allowlisted [Object class]
method on the object backing a [Key], returning an output blob. The data-plane mapping
of RADOS `rados_exec` onto NVMe KV.
_Avoid_: KV RPC, compute command.

**Object class** (cls):
A RADOS server-side plugin method executed on the OSD that owns an object, invoked via
`rados_exec`. KV Exec targets one of these.
_Avoid_: plugin, coprocessor, UDF.

**Op-ID**:
The small integer a [KV Exec] command carries to name its operation. The [Control
sidecar] maps an op-ID to a concrete (object class, method, arg policy); initiator
strings never name the class/method directly.
_Avoid_: opcode (that's the NVMe opcode), method id.

**Allowlist**:
The set of permitted [Op-ID] → (object class, method) bindings the target will execute.
The trust boundary for [KV Exec]; managed by the [Control sidecar].
_Avoid_: whitelist, ACL.

**Control sidecar**:
The external gRPC service that exposes get/set of the [Allowlist] and applies it to the
SPDK target over SPDK's JSON-RPC. Separate from the data plane.
_Avoid_: controller (overloaded with NVMe controller), gRPC server (when precision needed).
