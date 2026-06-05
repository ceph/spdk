/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"
#include "spdk/log.h"
#include "spdk/init.h"

#include "kvdev_mem.h"

static void
kvdev_subsystem_initialize(void)
{
	int rc;

	rc = spdk_kvdev_initialize();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize kvdev subsystem\n");
	}

	spdk_subsystem_init_next(rc);
}

static void
kvdev_subsystem_finish(void)
{
	spdk_kvdev_finish();
	spdk_subsystem_fini_next();
}

static void
kvdev_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);

	/* Emit one kvdev_mem_create per in-memory kvdev so a saved config can be
	 * reloaded. KV namespaces (nvmf_subsystem_add_kv_ns) reference these by
	 * name, and the nvmf subsystem depends on kvdev so this section is
	 * emitted/replayed first. */
	kvdev_mem_write_config_json(w);

	spdk_json_write_array_end(w);
}

static struct spdk_subsystem g_spdk_subsystem_kvdev = {
	.name = "kvdev",
	.init = kvdev_subsystem_initialize,
	.fini = kvdev_subsystem_finish,
	.write_config_json = kvdev_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_kvdev);
