# Map each subsystem to a rados pool and each namespace to a rados namespace

For the librados backend, each NVMf **subsystem** maps to one distinct rados **pool**,
and each KV **namespace** within it maps to one rados **namespace** inside that pool
(set on the ioctx via `rados_ioctx_set_namespace`). Object-per-KV oids live within that
pool+namespace scope, so identical keys in different NVMe namespaces never collide.

## Why

Pools give strong isolation and independent placement/replication/quota policy per
subsystem, and rados namespaces give cheap per-NVMe-namespace key-space separation
without extra pools. It is the most direct mapping of the NVMe tenancy hierarchy onto
rados primitives.

## Consequences / caveat

- **Pools are not free in Ceph.** Each pool carries placement-group overhead, so
  "one pool per subsystem" does not scale to very large subsystem counts the way rados
  namespaces (which are nearly free) do. This is acceptable for the intended scale but
  is the main reason a future design might instead collapse subsystems onto shared
  pools distinguished by namespace. Pools are operator-pre-provisioned (see
  [0002](0002-rados-object-per-kv-pair.md)); the backend does not create them.
