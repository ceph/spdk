/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_KVDEV_MEM_H
#define SPDK_KVDEV_MEM_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/kvdev.h"
#include "spdk/json.h"

struct kvdev_mem_opts {
	const char		*name;
	struct spdk_uuid	uuid;
	/* Maximum value length in bytes. 0 selects a default. */
	uint32_t		max_value_len;
	/* Maximum number of keys (0 = unlimited). */
	uint32_t		max_num_keys;
};

/**
 * Create an in-memory kvdev instance.
 *
 * \param opts Creation options.
 * \param kvdev Output pointer to the created kvdev.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_mem_create(const struct kvdev_mem_opts *opts, struct spdk_kvdev **kvdev);

/**
 * Delete an in-memory kvdev instance by name.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_mem_delete(const char *name);

/**
 * Debug snapshot of a stored entry. Used by the kvdev_mem_get_entry RPC to let
 * tests inspect persisted state (notably the store-only vendor TTL, ADR-0003).
 */
struct kvdev_mem_entry_info {
	uint32_t	value_len;
	/** Whether a TTL was recorded for this entry. */
	bool		ttl_valid;
	/** Requested TTL in seconds (valid only when ttl_valid). */
	uint32_t	ttl;
	/** Absolute unix expiry deadline (store time + ttl); 0 if no TTL. */
	uint64_t	deadline;
};

/**
 * Look up a stored entry by key and fill in a debug snapshot. Intended for
 * tests/diagnostics, not the hot path.
 *
 * \return 0 on success, -ENODEV if the kvdev is unknown, -ENOENT if the key is
 * absent, -EINVAL on bad arguments.
 */
int kvdev_mem_get_entry(const char *name, const void *key, uint8_t key_len,
			struct kvdev_mem_entry_info *info);

/**
 * Emit JSON-RPC methods (kvdev_mem_create) that recreate all current in-memory
 * kvdevs. Called by the kvdev event subsystem's write_config_json so a saved
 * config can be reloaded.
 */
void kvdev_mem_write_config_json(struct spdk_json_write_ctx *w);

#endif /* SPDK_KVDEV_MEM_H */
