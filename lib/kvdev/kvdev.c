/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/kvdev.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"

struct spdk_kvdev_desc {
	struct spdk_kvdev		*kvdev;
	bool				write;
	TAILQ_ENTRY(spdk_kvdev_desc)	link;
};

static TAILQ_HEAD(, spdk_kvdev) g_kvdevs = TAILQ_HEAD_INITIALIZER(g_kvdevs);
static TAILQ_HEAD(, spdk_kvdev_module) g_kvdev_modules =
	TAILQ_HEAD_INITIALIZER(g_kvdev_modules);
static bool g_kvdev_initialized;

void
spdk_kvdev_module_list_add(struct spdk_kvdev_module *module)
{
	struct spdk_kvdev_module *m;

	TAILQ_FOREACH(m, &g_kvdev_modules, internal_link) {
		if (m == module) {
			SPDK_ERRLOG("kvdev module '%s' already registered\n", module->name);
			assert(false);
			return;
		}
	}

	TAILQ_INSERT_TAIL(&g_kvdev_modules, module, internal_link);
}

int
spdk_kvdev_initialize(void)
{
	struct spdk_kvdev_module *module;
	int rc;

	if (g_kvdev_initialized) {
		return 0;
	}

	TAILQ_FOREACH(module, &g_kvdev_modules, internal_link) {
		if (module->module_init) {
			rc = module->module_init();
			if (rc != 0) {
				SPDK_ERRLOG("kvdev module '%s' init failed: %d\n", module->name, rc);
				return rc;
			}
		}
	}

	g_kvdev_initialized = true;
	return 0;
}

void
spdk_kvdev_finish(void)
{
	struct spdk_kvdev_module *module;

	if (!g_kvdev_initialized) {
		return;
	}

	TAILQ_FOREACH(module, &g_kvdev_modules, internal_link) {
		if (module->module_fini) {
			module->module_fini();
		}
	}

	g_kvdev_initialized = false;
}

struct spdk_kvdev *
spdk_kvdev_get_by_name(const char *name)
{
	struct spdk_kvdev *kvdev;

	TAILQ_FOREACH(kvdev, &g_kvdevs, internal_link) {
		if (strcmp(kvdev->name, name) == 0) {
			return kvdev;
		}
	}

	return NULL;
}

struct spdk_kvdev *
spdk_kvdev_first(void)
{
	return TAILQ_FIRST(&g_kvdevs);
}

struct spdk_kvdev *
spdk_kvdev_next(struct spdk_kvdev *prev)
{
	return TAILQ_NEXT(prev, internal_link);
}

int
spdk_kvdev_register(struct spdk_kvdev *kvdev)
{
	if (kvdev == NULL || kvdev->name == NULL || kvdev->fn_table == NULL) {
		return -EINVAL;
	}

	if (kvdev->fn_table->get_io_channel == NULL ||
	    kvdev->fn_table->store == NULL ||
	    kvdev->fn_table->retrieve == NULL) {
		SPDK_ERRLOG("kvdev '%s' fn_table is missing required ops\n", kvdev->name);
		return -EINVAL;
	}

	if (kvdev->caps.max_key_len == 0 ||
	    kvdev->caps.max_key_len > SPDK_KVDEV_KEY_MAX_LEN) {
		SPDK_ERRLOG("kvdev '%s' has invalid max_key_len %u\n", kvdev->name,
			    kvdev->caps.max_key_len);
		return -EINVAL;
	}

	if (spdk_kvdev_get_by_name(kvdev->name) != NULL) {
		SPDK_ERRLOG("kvdev '%s' already exists\n", kvdev->name);
		return -EEXIST;
	}

	if (spdk_uuid_is_null(&kvdev->uuid)) {
		spdk_uuid_generate(&kvdev->uuid);
	}

	TAILQ_INIT(&kvdev->open_descs);
	TAILQ_INSERT_TAIL(&g_kvdevs, kvdev, internal_link);

	SPDK_DEBUGLOG(kvdev, "Registered kvdev '%s'\n", kvdev->name);
	return 0;
}

int
spdk_kvdev_unregister(struct spdk_kvdev *kvdev)
{
	if (kvdev == NULL) {
		return -EINVAL;
	}

	if (!TAILQ_EMPTY(&kvdev->open_descs)) {
		SPDK_ERRLOG("kvdev '%s' still has open descriptors\n", kvdev->name);
		return -EBUSY;
	}

	TAILQ_REMOVE(&g_kvdevs, kvdev, internal_link);

	if (kvdev->fn_table->destruct) {
		kvdev->fn_table->destruct(kvdev->ctxt);
	}

	return 0;
}

int
spdk_kvdev_open(const char *name, bool write, struct spdk_kvdev_desc **_desc)
{
	struct spdk_kvdev *kvdev;
	struct spdk_kvdev_desc *desc;

	kvdev = spdk_kvdev_get_by_name(name);
	if (kvdev == NULL) {
		return -ENODEV;
	}

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		return -ENOMEM;
	}

	desc->kvdev = kvdev;
	desc->write = write;
	TAILQ_INSERT_TAIL(&kvdev->open_descs, desc, link);

	*_desc = desc;
	return 0;
}

void
spdk_kvdev_close(struct spdk_kvdev_desc *desc)
{
	if (desc == NULL) {
		return;
	}

	TAILQ_REMOVE(&desc->kvdev->open_descs, desc, link);
	free(desc);
}

struct spdk_kvdev *
spdk_kvdev_desc_get_kvdev(struct spdk_kvdev_desc *desc)
{
	return desc->kvdev;
}

const char *
spdk_kvdev_get_name(const struct spdk_kvdev *kvdev)
{
	return kvdev->name;
}

const struct spdk_kvdev_caps *
spdk_kvdev_get_caps(const struct spdk_kvdev *kvdev)
{
	return &kvdev->caps;
}

struct spdk_io_channel *
spdk_kvdev_get_io_channel(struct spdk_kvdev_desc *desc)
{
	struct spdk_kvdev *kvdev = desc->kvdev;

	return kvdev->fn_table->get_io_channel(kvdev->ctxt);
}

int
spdk_kvdev_store(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		 const void *key, uint8_t key_len,
		 const void *value, uint32_t value_len,
		 const struct spdk_kvdev_store_opts *opts,
		 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_kvdev *kvdev = desc->kvdev;

	if (key == NULL || key_len < SPDK_KVDEV_KEY_MIN_LEN || key_len > kvdev->caps.max_key_len) {
		return -EINVAL;
	}

	return kvdev->fn_table->store(ch, key, key_len, value, value_len, opts, cb_fn, cb_arg);
}

int
spdk_kvdev_retrieve(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		    const void *key, uint8_t key_len,
		    void *value_buf, uint32_t buf_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_kvdev *kvdev = desc->kvdev;

	if (key == NULL || key_len < SPDK_KVDEV_KEY_MIN_LEN || key_len > kvdev->caps.max_key_len) {
		return -EINVAL;
	}

	return kvdev->fn_table->retrieve(ch, key, key_len, value_buf, buf_len, cb_fn, cb_arg);
}

SPDK_LOG_REGISTER_COMPONENT(kvdev)
