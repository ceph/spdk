# Back NVMe KV namespaces with a new `kvdev` abstraction, not `bdev`

An NVMf namespace in SPDK is always backed by a `spdk_bdev`, whose interface is
block-shaped (LBA, block size, block count, ZCOPY, optimal-IO-boundary). The NVMe
Key-Value Command Set is object-shaped (a 1–16 byte binary key maps to a
variable-length value) and satisfies almost none of the block contract. ZNS could
reuse `bdev` because zones are still block storage; KV genuinely is not.

We decided to introduce a new `kvdev` library — a sibling to `bdev` with its own
module registration and descriptors — and give the NVMf target a KV-namespace path
that binds to a kvdev. Concrete modules (in-memory first, librados later) implement
the kvdev interface.

## Considered Options

- **Dedicated `kv_backend` vtable inside NVMf** — smallest code, but bespoke and not
  reusable outside the target.
- **Extend `bdev` with KV IO types** — matches the "backends are bdev modules"
  convention, but forces KV modules to stub block-shaped fields they do not have and
  pollutes a block interface for every existing bdev consumer.
- **New parallel `kvdev` subsystem (chosen)** — cleanest conceptual model and fully
  reusable (e.g. a future kvdev fio plugin); cost is the largest amount of new code
  and upstream surface (new lib, RPCs, descriptors, fio engine).

## Consequences

- New public surface: a `kvdev` library, its RPCs, and a kvdev-aware fio engine.
- The NVMf target gains a second namespace-backing path alongside the bdev path.
- Slowest of the three options to a working demo, but lowest long-term friction.
