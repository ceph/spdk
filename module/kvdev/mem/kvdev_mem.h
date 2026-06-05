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
 * Emit JSON-RPC methods (kvdev_mem_create) that recreate all current in-memory
 * kvdevs. Called by the kvdev event subsystem's write_config_json so a saved
 * config can be reloaded.
 */
void kvdev_mem_write_config_json(struct spdk_json_write_ctx *w);

#endif /* SPDK_KVDEV_MEM_H */
