/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/tree.h"
#include "spdk/queue_extras.h"
#include "spdk/util.h"
#include "spdk/json.h"

#include "kvdev_mem.h"

/* Default maximum value length for an in-memory kvdev (1 MiB). */
#define KVDEV_MEM_DEFAULT_MAX_VALUE_LEN (1024 * 1024)

/*
 * Built-in KV Exec operations (vendor extension, ADR-0005). The in-memory
 * backend selects an operation by this small integer op-ID; there is no
 * allowlist yet (KVX-2) -- these built-ins are unconditional for now.
 *   ECHO:   copy the input blob straight to the output (key need not exist).
 *   APPEND: append the input blob to the value stored under the key and return
 *           the new full value as output; reports the new value length.
 */
enum kvdev_mem_exec_op {
	KVDEV_MEM_EXEC_OP_ECHO		= 1,
	KVDEV_MEM_EXEC_OP_APPEND	= 2,
};

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
	/*
	 * Vendor TTL (ADR-0003), store-only. When ttl_valid is set, ttl is the
	 * requested time-to-live in seconds and deadline is the absolute unix
	 * expiry time (store time + ttl). Recorded but never enforced: a
	 * Retrieve after the deadline still returns the value.
	 */
	bool				ttl_valid;
	uint32_t			ttl;
	time_t				deadline;
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

static void kvdev_mem_module_fini(void);

static struct spdk_kvdev_module g_kvdev_mem_module = {
	.name = "kvdev_mem",
	.module_fini = kvdev_mem_module_fini,
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

/*
 * Read the store flags from an extensible options struct, honouring its size
 * field so a caller built against an older/newer header is handled safely.
 */
static uint32_t
kvdev_mem_store_flags(const struct spdk_kvdev_store_opts *opts)
{
	if (opts == NULL || opts->size < offsetof(struct spdk_kvdev_store_opts, flags) +
	    sizeof(opts->flags)) {
		return SPDK_KVDEV_STORE_FLAG_NONE;
	}
	return opts->flags;
}

/*
 * Read the vendor TTL (ADR-0003) from an extensible options struct, honouring
 * its size field. Returns true and fills *ttl when the caller both supplied a
 * struct large enough to carry ttl and set SPDK_KVDEV_STORE_F_TTL; false
 * otherwise (no TTL requested / older caller).
 */
static bool
kvdev_mem_store_ttl(const struct spdk_kvdev_store_opts *opts, uint32_t *ttl)
{
	if (opts == NULL ||
	    !(kvdev_mem_store_flags(opts) & SPDK_KVDEV_STORE_F_TTL) ||
	    opts->size < offsetof(struct spdk_kvdev_store_opts, ttl) + sizeof(opts->ttl)) {
		return false;
	}
	*ttl = opts->ttl;
	return true;
}

/*
 * Record (or clear) the store-only TTL/deadline on an entry. Never enforced.
 */
static void
kvdev_mem_entry_set_ttl(struct kvdev_mem_entry *entry,
			const struct spdk_kvdev_store_opts *opts)
{
	uint32_t ttl;

	if (kvdev_mem_store_ttl(opts, &ttl)) {
		entry->ttl_valid = true;
		entry->ttl = ttl;
		entry->deadline = time(NULL) + (time_t)ttl;
	} else {
		entry->ttl_valid = false;
		entry->ttl = 0;
		entry->deadline = 0;
	}
}

static int
kvdev_mem_store(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		const void *value, uint32_t value_len,
		const struct spdk_kvdev_store_opts *opts,
		spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;
	struct kvdev_mem_entry *entry;
	uint32_t flags = kvdev_mem_store_flags(opts);
	void *buf;

	if (value_len > mdev->kvdev.caps.max_value_len) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry != NULL) {
		/* Store-If-No-Key-Exists (SINKE): the key already exists, so reject. */
		if (flags & SPDK_KVDEV_STORE_FLAG_SINKE) {
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_KEY_EXIST, 0);
			return 0;
		}
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
		kvdev_mem_entry_set_ttl(entry, opts);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
		return 0;
	}

	/* Store-If-Key-Exists (SIKE): the key is absent, so reject. */
	if (flags & SPDK_KVDEV_STORE_FLAG_SIKE) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0);
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
	kvdev_mem_entry_set_ttl(entry, opts);

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
kvdev_mem_op_delete(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;
	struct kvdev_mem_entry *entry;

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0);
		return 0;
	}

	RB_REMOVE(kvdev_mem_tree, &mdev->tree, entry);
	mdev->num_keys--;
	free(entry->value);
	free(entry);

	cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
	return 0;
}

static int
kvdev_mem_exist(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;
	struct kvdev_mem_entry *entry;

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0);
		return 0;
	}

	cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
	return 0;
}

static int
kvdev_mem_list(struct spdk_io_channel *ch, const void *start_key, uint8_t start_key_len,
	       spdk_kvdev_list_cb iter_cb, void *iter_arg,
	       spdk_kvdev_list_done_cb done_cb, void *done_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;
	struct kvdev_mem_entry *entry;
	uint32_t num_keys = 0;

	/*
	 * The RB tree is ordered by kvdev_mem_entry_cmp(), which gives the
	 * unspecified-but-stable iteration order the List contract requires.
	 * start_key is a position into that order:
	 *   - NULL/empty: start at the very first key (RB_MIN).
	 *   - present:    start at that key.
	 *   - absent:     start at the first key that sorts at-or-after it
	 *                 (RB_NFIND), a stable vendor-specific start point.
	 */
	if (start_key == NULL || start_key_len == 0) {
		entry = RB_MIN(kvdev_mem_tree, &mdev->tree);
	} else {
		struct kvdev_mem_entry find = {};

		memcpy(find.key, start_key, start_key_len);
		find.key_len = start_key_len;
		entry = RB_NFIND(kvdev_mem_tree, &mdev->tree, &find);
	}

	for (; entry != NULL; entry = RB_NEXT(kvdev_mem_tree, &mdev->tree, entry)) {
		if (!iter_cb(iter_arg, entry->key, entry->key_len)) {
			/* Consumer is full (e.g. next key would not fit). Stop here;
			 * this key was NOT accepted. */
			break;
		}
		num_keys++;
	}

	done_cb(done_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, num_keys);
	return 0;
}

/*
 * Emit an exec result with the Retrieve-style truncation contract: copy at most
 * buf_len bytes of the true output into output_buf, but always report the true
 * length. status is BUFFER_TOO_SMALL when the output did not fully fit.
 */
static void
kvdev_mem_exec_emit(void *output_buf, uint32_t buf_len, const void *out, uint32_t out_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	uint32_t copy_len = spdk_min(out_len, buf_len);

	/* The caller may pass the same buffer for input and output (echo over the
	 * single bidirectional data buffer), so use memmove and skip the copy when
	 * source and destination already coincide. */
	if (copy_len > 0 && output_buf != out) {
		memmove(output_buf, out, copy_len);
	}

	if (out_len > buf_len) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL, out_len);
	} else {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, out_len);
	}
}

/*
 * KV Exec built-in: APPEND. Append the input blob to the value currently stored
 * under the key (the key must exist) and return the new full value as output.
 * The stored value grows by input_len; the output length reported is the new
 * value length even if the host buffer could not hold all of it.
 */
static int
kvdev_mem_exec_append(struct kvdev_mem *mdev, const void *key, uint8_t key_len,
		      const void *input, uint32_t input_len,
		      void *output_buf, uint32_t output_buf_len,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_entry *entry;
	uint32_t new_len;
	void *buf;

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0);
		return 0;
	}

	new_len = entry->value_len + input_len;
	if (new_len > mdev->kvdev.caps.max_value_len) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	buf = malloc(new_len ? new_len : 1);
	if (buf == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	if (entry->value_len > 0) {
		memcpy(buf, entry->value, entry->value_len);
	}
	if (input_len > 0) {
		memcpy((uint8_t *)buf + entry->value_len, input, input_len);
	}

	free(entry->value);
	entry->value = buf;
	entry->value_len = new_len;

	kvdev_mem_exec_emit(output_buf, output_buf_len, buf, new_len, cb_fn, cb_arg);
	return 0;
}

static int
kvdev_mem_exec(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
	       uint32_t op_id, const void *input, uint32_t input_len,
	       void *output_buf, uint32_t output_buf_len,
	       spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_mem_io_channel *mch = spdk_io_channel_get_ctx(ch);
	struct kvdev_mem *mdev = mch->mdev;

	switch (op_id) {
	case KVDEV_MEM_EXEC_OP_ECHO:
		/* Echo the input blob straight back as the output. */
		kvdev_mem_exec_emit(output_buf, output_buf_len, input, input_len, cb_fn, cb_arg);
		return 0;
	case KVDEV_MEM_EXEC_OP_APPEND:
		return kvdev_mem_exec_append(mdev, key, key_len, input, input_len,
					     output_buf, output_buf_len, cb_fn, cb_arg);
	default:
		/* Unknown op-ID: rejected (no allowlist yet, KVX-2). */
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}
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
	.del		= kvdev_mem_op_delete,
	.exist		= kvdev_mem_exist,
	.list		= kvdev_mem_list,
	.exec		= kvdev_mem_exec,
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
kvdev_mem_get_entry(const char *name, const void *key, uint8_t key_len,
		    struct kvdev_mem_entry_info *info)
{
	struct spdk_kvdev *kvdev;
	struct kvdev_mem *mdev;
	struct kvdev_mem_entry *entry;

	if (name == NULL || key == NULL || info == NULL ||
	    key_len < SPDK_KVDEV_KEY_MIN_LEN || key_len > SPDK_KVDEV_KEY_MAX_LEN) {
		return -EINVAL;
	}

	kvdev = spdk_kvdev_get_by_name(name);
	if (kvdev == NULL || kvdev->module != &g_kvdev_mem_module) {
		return -ENODEV;
	}
	mdev = SPDK_CONTAINEROF(kvdev, struct kvdev_mem, kvdev);

	entry = kvdev_mem_find(mdev, key, key_len);
	if (entry == NULL) {
		return -ENOENT;
	}

	info->value_len = entry->value_len;
	info->ttl_valid = entry->ttl_valid;
	info->ttl = entry->ttl;
	info->deadline = (uint64_t)entry->deadline;
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

/*
 * Module teardown: destroy any in-memory kvdevs still alive at shutdown so
 * their per-kvdev spdk_io_device is unregistered (otherwise
 * spdk_thread_lib_fini logs "io_device <name> not unregistered").  Reuses the
 * normal kvdev_mem_delete() teardown path, which unregisters the io_device and
 * frees state.  Idempotent and a no-op when no kvdevs remain.
 */
static void
kvdev_mem_module_fini(void)
{
	struct kvdev_mem *mdev, *tmp;

	TAILQ_FOREACH_SAFE(mdev, &g_kvdev_mem_head, tailq, tmp) {
		kvdev_mem_delete(mdev->kvdev.name);
	}
}

void
kvdev_mem_write_config_json(struct spdk_json_write_ctx *w)
{
	struct kvdev_mem *mdev;
	const struct spdk_kvdev_caps *caps;

	TAILQ_FOREACH(mdev, &g_kvdev_mem_head, tailq) {
		caps = &mdev->kvdev.caps;

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "kvdev_mem_create");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", mdev->kvdev.name);
		if (caps->max_value_len != 0) {
			spdk_json_write_named_uint32(w, "max_value_len", caps->max_value_len);
		}
		if (caps->max_num_keys != 0) {
			spdk_json_write_named_uint32(w, "max_num_keys", caps->max_num_keys);
		}
		if (!spdk_uuid_is_null(&mdev->kvdev.uuid)) {
			spdk_json_write_named_uuid(w, "uuid", &mdev->kvdev.uuid);
		}
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT(kvdev_mem)
