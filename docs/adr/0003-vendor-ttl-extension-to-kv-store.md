# Vendor TTL extension to the KV Store command (store-only)

We add a non-standard, vendor-specific TTL to the Key-Value **Store** command. The
ratified KV spec has no TTL, so this is a deliberate deviation.

## Transport (command encoding)

- **CDW12** carries the TTL in seconds (a 32-bit field; Store does not otherwise use
  CDW12 per spec Figures 29–33).
- A reserved **Store Option** bit (CDW11 bits 15:11) is defined as **TTL Valid** — when
  set, CDW12 is honored; when clear, no TTL is applied (backward compatible with hosts
  that don't know about it).
- Support is advertised via a capability bit in the KV Identify data so a host can
  detect it.
- Host API: a new `spdk_nvme_kv_store_ext()` taking a size-versioned opts struct that
  carries the TTL. The existing `spdk_nvme_kv_store()` ABI (already exported in
  `lib/nvme/spdk_nvme.map`) is left unchanged.

## Behavior: store-only, NOT enforced

The target persists the TTL as a deadline and does nothing else with it. In the
librados backend this is an **xattr** on the KV object; the in-memory module stores it
alongside the value for parity.

A Retrieve after the deadline **still returns the value** — expiry is not enforced in
this iteration (no lazy expiry on access, no reaper). Enforcement is left to a future
feature or external tooling.

## Why

Get the command path (host → target → backend) and the metadata persistence working
first, cheaply, with no dependence on the deferred List/enumeration and no target-clock
/ expiry-reclamation machinery. Active enforcement (lazy-on-access or a reaper) is a
separate, later decision.

## Considered Options

- **Lazy expiry on access** — correct TTL semantics without enumeration, but adds a
  target clock and inline-delete path; deferred.
- **Active reaper** — true space reclamation, but needs the enumeration we deferred for
  rados; rejected for now.
