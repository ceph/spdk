/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/tree.h"
#include "spdk/util.h"

#include "kvdev_mem.h"

/* Default maximum value length for an in-memory kvdev (1 MiB). */
#define KVDEV_MEM_DEFAULT_MAX_VALUE_LEN (1024 * 1024)

/*
 * A single key->value entry.  The key is a short binary blob (1-16 bytes) and is
 * stored inline; the value is a separately allocated buffer.  Entries live in an
 * RB tree keyed by (key_len, key bytes) so that store/retrieve are O(log n).
 */
struct kvdev_mem_entry {
	RB_ENTRY(kvdev_mem_entry)	link;
	uint8_t				key[SPDK_KVDEV_KEY_MAX_LEN];
	uint8_t				key_len;
	uint32_t			value_len;
	void				*value;
};

struct kvdev_mem {
	struct spdk_kvdev				kvdev;
	RB_HEAD(kvdev_mem_tree, kvdev_mem_entry)	tree;
	uint32_t					num_keys;
	TAILQ_ENTRY(kvdev_mem)				tailq;
};

/* Per-thread io_channel. The store is global (single-threaded in this slice),
 * so the channel itself carries no state yet, but keeping it mirrors the bdev
 * runtime model and gives later slices a home for per-thread queues. */
struct kvdev_mem_io_channel {
	struct kvdev_mem	*mdev;
};

static TAILQ_HEAD(, kvdev_mem) g_kvdev_mem_head = TAILQ_HEAD_INITIALIZER(g_kvdev_mem_head);

static struct spdk_kvdev_module g_kvdev_mem_module = {
	.name = "kvdev_mem",
};

SPDK_KVDEV_MODULE_REGISTER(kvdev_mem, &g_kvdev_mem_module)

static int
kvdev_mem_entry_cmp(struct kvdev_mem_entry *a, struct kvdev_mem_entry *b)
{
	int rc;
	uint8_t min_len = spdk_min(a->key_len, b->key_len);

	rc = memcmp(a->key, b->key, min_len);
	if (rc != 0) {
		return rc;
	}

	/* Shared prefix; the shorter key sorts first. */
	if (a->key_len < b->key_len) {
		return -1;
	}
	if (a->key_len > b->key_len) {
		return 1;
	}
	return 0;
}

RB_GENERATE_STATIC(kvdev_mem_tree, kvdev_mem_entry, link, kvdev_mem_entry_cmp);

static struct kvdev_mem_entry *
kvdev_mem_find(struct kvdev_mem *mdev, const void *key, uint8_t key_len)
{
	struct kvdev_mem_entry find = {};

	memcpy(find.key, key, key_len);
	find.key_len = key_len;

	return RB_FIND(kvdev_mem_tree, &mdev->tree, &find);
}

static int
kvdev_mem_store(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		const void *value, uint32_t value_len,
		spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;
	struct kvdev_mem_entry *entry;
	void *buf;

	if (value_len > mdev->kvdev.caps.max_value_len) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry != NULL) {
		/* Overwrite existing value. */
		buf = malloc(value_len ? value_len : 1);
		if (buf == NULL) {
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
			return 0;
		}
		memcpy(buf, value, value_len);
		free(entry->value);
		entry->value = buf;
		entry->value_len = value_len;
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
		return 0;
	}

	/* New key. */
	if (mdev->kvdev.caps.max_num_keys != 0 &&
	    mdev->num_keys >= mdev->kvdev.caps.max_num_keys) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	buf = malloc(value_len ? value_len : 1);
	if (buf == NULL) {
		free(entry);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	memcpy(entry->key, key, key_len);
	entry->key_len = key_len;
	memcpy(buf, value, value_len);
	entry->value = buf;
	entry->value_len = value_len;

	RB_INSERT(kvdev_mem_tree, &mdev->tree, entry);
	mdev->num_keys++;

	cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
	return 0;
}

static int
kvdev_mem_retrieve(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		   void *value_buf, uint32_t buf_len,
		   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;
	struct kvdev_mem_entry *entry;
	uint32_t copy_len;

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0);
		return 0;
	}

	copy_len = spdk_min(entry->value_len, buf_len);
	if (copy_len > 0) {
		memcpy(value_buf, entry->value, copy_len);
	}

	if (entry->value_len > buf_len) {
		/* Report the true length so the host knows the value was truncated. */
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL, entry->value_len);
		return 0;
	}

	cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, entry->value_len);
	return 0;
}

static int
kvdev_mem_create_channel_cb(void *io_device, void *ctx_buf)
{
	struct kvdev_mem_io_channel *mch = ctx_buf;

	mch->mdev = io_device;
	return 0;
}

static void
kvdev_mem_destroy_channel_cb(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *
kvdev_mem_get_io_channel(void *ctx)
{
	struct kvdev_mem *mdev = ctx;

	return spdk_get_io_channel(mdev);
}

static void
kvdev_mem_free(struct kvdev_mem *mdev)
{
	struct kvdev_mem_entry *entry, *tmp;

	RB_FOREACH_SAFE(entry, kvdev_mem_tree, &mdev->tree, tmp) {
		RB_REMOVE(kvdev_mem_tree, &mdev->tree, entry);
		free(entry->value);
		free(entry);
	}

	free(mdev->kvdev.name);
	free(mdev);
}

static void
kvdev_mem_unregister_io_device_done(void *io_device)
{
	struct kvdev_mem *mdev = io_device;

	kvdev_mem_free(mdev);
}

static int
kvdev_mem_destruct(void *ctx)
{
	struct kvdev_mem *mdev = ctx;

	TAILQ_REMOVE(&g_kvdev_mem_head, mdev, tailq);
	spdk_io_device_unregister(mdev, kvdev_mem_unregister_io_device_done);
	return 0;
}

static const struct spdk_kvdev_fn_table kvdev_mem_fn_table = {
	.destruct	= kvdev_mem_destruct,
	.get_io_channel	= kvdev_mem_get_io_channel,
	.store		= kvdev_mem_store,
	.retrieve	= kvdev_mem_retrieve,
};

int
kvdev_mem_create(const struct kvdev_mem_opts *opts, struct spdk_kvdev **_kvdev)
{
	struct kvdev_mem *mdev;
	uint32_t max_value_len;
	int rc;

	if (opts == NULL || opts->name == NULL) {
		return -EINVAL;
	}

	max_value_len = opts->max_value_len ? opts->max_value_len : KVDEV_MEM_DEFAULT_MAX_VALUE_LEN;

	mdev = calloc(1, sizeof(*mdev));
	if (mdev == NULL) {
		return -ENOMEM;
	}

	RB_INIT(&mdev->tree);

	mdev->kvdev.name = strdup(opts->name);
	if (mdev->kvdev.name == NULL) {
		free(mdev);
		return -ENOMEM;
	}

	mdev->kvdev.ctxt = mdev;
	mdev->kvdev.fn_table = &kvdev_mem_fn_table;
	mdev->kvdev.module = &g_kvdev_mem_module;
	mdev->kvdev.caps.max_key_len = SPDK_KVDEV_KEY_MAX_LEN;
	mdev->kvdev.caps.max_value_len = max_value_len;
	mdev->kvdev.caps.max_num_keys = opts->max_num_keys;
	if (!spdk_uuid_is_null(&opts->uuid)) {
		spdk_uuid_copy(&mdev->kvdev.uuid, &opts->uuid);
	}

	/* Register the io_device before the kvdev so a channel can be obtained
	 * immediately after spdk_kvdev_register() returns. */
	spdk_io_device_register(mdev, kvdev_mem_create_channel_cb, kvdev_mem_destroy_channel_cb,
				sizeof(struct kvdev_mem_io_channel), opts->name);

	rc = spdk_kvdev_register(&mdev->kvdev);
	if (rc != 0) {
		spdk_io_device_unregister(mdev, NULL);
		free(mdev->kvdev.name);
		free(mdev);
		return rc;
	}

	TAILQ_INSERT_TAIL(&g_kvdev_mem_head, mdev, tailq);

	*_kvdev = &mdev->kvdev;
	SPDK_DEBUGLOG(kvdev_mem, "Created in-memory kvdev '%s' (max_value_len=%u, max_num_keys=%u)\n",
		      opts->name, max_value_len, opts->max_num_keys);
	return 0;
}

int
kvdev_mem_delete(const char *name)
{
	struct spdk_kvdev *kvdev;

	kvdev = spdk_kvdev_get_by_name(name);
	if (kvdev == NULL) {
		return -ENODEV;
	}

	if (kvdev->module != &g_kvdev_mem_module) {
		SPDK_ERRLOG("kvdev '%s' is not an in-memory kvdev\n", name);
		return -EINVAL;
	}

	return spdk_kvdev_unregister(kvdev);
}

SPDK_LOG_REGISTER_COMPONENT(kvdev_mem)
