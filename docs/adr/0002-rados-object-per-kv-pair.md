# librados kvdev: one rados object per KV pair

For the librados-backed kvdev module, a KV pair maps to a single rados object: the
Key becomes the object name (oid) and the Value becomes the object data (written with
`aio_write_full`, read with `aio_read`). Exist maps to `aio_stat`, Delete to
`aio_remove`. This is the natural, scalable fit — values may be large and are
distributed across PGs/OSDs — and it composes cleanly with the subsystem→pool and
namespace→rados-namespace mapping (the oid lives inside the namespace's rados
namespace within the subsystem's pool).

The alternatives (all keys as omap entries under one index object; or a hybrid of
object-data values plus an omap index) were rejected for the first cut: omap funnels
every key through a single object (hotspot, size limits, large values hurt), and the
hybrid adds a dual-write consistency burden.

## Value size limit

The librados backend caps the KV **value** size at **64 MB**, matching the rados
object-size limit (a value is one rados object's data; `osd_max_object_size` defaults
to 128 MiB, and 64 MB is the conservative project cap). This maximum is advertised to
the initiator as the per-namespace max value length (`kvvml` in the KV Identify
namespace data). **No `libradosstriper`** — a value is never split across multiple
rados objects, so a Store exceeding the cap is rejected rather than striped. (Keys are
unaffected: NVMe KV keys are ≤16 bytes by spec.)

## Consequences

- The **List** command is **deferred** for the librados backend. rados object
  enumeration (`nobjects_list`) is an unordered cursor that cannot seek to the spec's
  start-*key*, so spec-conformant, paginated List on object-per-KV is non-trivial
  (O(n) per page) and is out of scope for the first Ceph cut. List remains in the
  kvdev interface and is implemented by the in-memory module; the librados module
  will return an appropriate status until a List strategy (e.g. a maintained omap
  index) is chosen later.
- Binary keys (1–16 bytes) must be encoded into a valid oid — see the open key-encoding
  decision.
