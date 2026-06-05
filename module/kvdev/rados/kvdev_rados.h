/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_KVDEV_RADOS_H
#define SPDK_KVDEV_RADOS_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/kvdev.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * librados-backed kvdev (ADR-0002, ADR-0004).
 *
 * Tenancy mapping:
 *   - subsystem -> rados pool (operator pre-provisioned; never auto-created)
 *   - KV namespace -> rados namespace (set on the per-instance ioctx)
 * Object-per-KV:
 *   - key   -> oid (hex-encoded, since the librados oid is a C string)
 *   - value -> object data (write_full / read / stat / remove)
 *
 * Clusters are shared and named: register one with kvdev_rados_register_cluster()
 * (mirrors bdev_rbd's cluster RPC) then create kvdevs that reference it by name.
 */

/* Options needed to register/connect a named rados cluster handle. */
struct kvdev_rados_cluster_info {
	const char		*name;		/* cluster handle name (required) */
	const char		*user_id;	/* ceph client id, e.g. "admin" (optional) */
	const char *const	*config_param;	/* NULL-terminated key,val,key,val,... (optional) */
	const char		*config_file;	/* ceph.conf path (optional) */
	const char		*key_file;	/* keyring path (optional) */
};

/* Options to create a rados-backed kvdev instance. */
struct kvdev_rados_opts {
	const char		*name;		/* kvdev name (required) */
	const char		*cluster_name;	/* named cluster handle (required) */
	const char		*pool_name;	/* rados pool == subsystem (required) */
	const char		*namespace_name;/* rados namespace == KV namespace (optional) */
	struct spdk_uuid	uuid;
	/* Max value length in bytes. 0 selects the ADR-0002 default (64 MB). */
	uint32_t		max_value_len;
};

/**
 * Register (create+connect) a named, shared rados cluster handle. Mirrors
 * bdev_rbd's cluster registry: the connect can fail (no monitor, bad auth) and
 * the error is returned to the caller without leaking. The handle is connected
 * on a non-SPDK thread to avoid contending with SPDK reactors.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_register_cluster(const struct kvdev_rados_cluster_info *info);

/**
 * Unregister a named cluster handle. Fails (-EBUSY) if any kvdev still
 * references it.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_unregister_cluster(const char *name);

/**
 * Dump cluster registry info as a JSON-RPC result. If \c name is NULL, all
 * clusters are emitted as an array; otherwise just the named one.
 *
 * \return 0 on success, -ENOENT if not found.
 */
int kvdev_rados_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name);

/**
 * Create a rados-backed kvdev. The referenced pool must already exist (pools are
 * operator pre-provisioned per ADR-0004).
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_create(const struct kvdev_rados_opts *opts, struct spdk_kvdev **kvdev);

/**
 * Delete a rados-backed kvdev by name.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_delete(const char *name);

/**
 * Emit JSON-RPC methods (cluster registrations + kvdev_rados_create) that
 * recreate the current rados kvdev state for config save/restore.
 */
void kvdev_rados_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KVDEV_RADOS_H */
