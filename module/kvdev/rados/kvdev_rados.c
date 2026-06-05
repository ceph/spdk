/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include <rados/librados.h>

#include "spdk/kvdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/queue.h"

#include "kvdev_rados.h"

/*
 * librados-backed kvdev. See kvdev_rados.h / ADR-0002 / ADR-0004 for the model.
 *
 * IO path is fully async and never blocks: each op builds a librados aio
 * completion and is queued on the submitting channel's in-flight list. A
 * per-channel poller harvests finished librados completions (rados_aio_is_complete)
 * and fires the kvdev completion callback on the SPDK thread — librados invokes
 * its own callbacks from internal threads, so we deliberately do NOT complete
 * the kvdev request from inside a librados callback.
 */

/* ADR-0002: cap value size at 64 MB; advertised as kvvml in KV Identify. */
#define KVDEV_RADOS_MAX_VALUE_LEN (64ull * 1024 * 1024)

/* xattr name for the store-only vendor TTL (ADR-0003 spirit; not enforced). */
#define KVDEV_RADOS_TTL_XATTR "kv_ttl"

/* librados oids are NUL-terminated C strings, so a binary key (1-16 bytes) is
 * hex-encoded: 16 bytes -> 32 hex chars + NUL. */
#define KVDEV_RADOS_OID_MAX (SPDK_KVDEV_KEY_MAX_LEN * 2 + 1)

/* ---- shared, named cluster registry (mirrors bdev_rbd) ------------------- */

struct kvdev_rados_cluster {
	char			*name;
	char			*user_id;
	char			**config_param;	/* NULL-terminated k,v,... */
	char			*config_file;
	char			*key_file;
	rados_t			cluster;
	uint32_t		ref;
	STAILQ_ENTRY(kvdev_rados_cluster) link;
};

static STAILQ_HEAD(, kvdev_rados_cluster) g_clusters =
	STAILQ_HEAD_INITIALIZER(g_clusters);
static pthread_mutex_t g_clusters_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Set during module_fini so the last put_cluster also shuts the cluster down
 * (kvdev destruct is async and may outlive the module_fini cluster sweep). */
static bool g_shutting_down = false;

/* ---- kvdev instance ------------------------------------------------------ */

struct kvdev_rados {
	struct spdk_kvdev		kvdev;
	char				*cluster_name;
	char				*pool_name;
	char				*namespace_name;
	rados_t				*cluster_p;	/* into the registry entry */
	rados_ioctx_t			io_ctx;		/* pool ioctx, namespace set */
	TAILQ_ENTRY(kvdev_rados)	tailq;
};

static TAILQ_HEAD(, kvdev_rados) g_kvdevs = TAILQ_HEAD_INITIALIZER(g_kvdevs);

struct kvdev_rados_io_channel {
	struct kvdev_rados	*rdev;
	struct spdk_poller	*poller;
	TAILQ_HEAD(, kvdev_rados_io) inflight;
};

enum kvdev_rados_op {
	KVDEV_RADOS_OP_STORE,
	KVDEV_RADOS_OP_RETRIEVE,
	KVDEV_RADOS_OP_DELETE,
	KVDEV_RADOS_OP_EXIST,
	KVDEV_RADOS_OP_EXEC,
};

/* One in-flight librados aio. Lives on the channel inflight list until the
 * poller observes its completion. */
struct kvdev_rados_io {
	enum kvdev_rados_op		op;
	rados_completion_t		comp;
	rados_write_op_t		write_op;	/* STORE only */
	rados_read_op_t			read_op;	/* RETRIEVE only */
	uint64_t			stat_size;	/* RETRIEVE/EXIST scratch */
	time_t				stat_mtime;	/* EXIST scratch */
	size_t				bytes_read;	/* RETRIEVE: read_op_read out */
	int				read_rval;	/* RETRIEVE: read_op_read rc */
	uint32_t			buf_len;	/* RETRIEVE/EXEC: caller buf size */
	spdk_kvdev_io_completion_cb	cb_fn;
	void				*cb_arg;
	TAILQ_ENTRY(kvdev_rados_io)	link;
};

static int kvdev_rados_module_init(void);
static void kvdev_rados_module_fini(void);

static struct spdk_kvdev_module g_kvdev_rados_module = {
	.name = "kvdev_rados",
	.module_init = kvdev_rados_module_init,
	.module_fini = kvdev_rados_module_fini,
};

SPDK_KVDEV_MODULE_REGISTER(kvdev_rados, &g_kvdev_rados_module)

/* ---- config helpers (dup/free a NULL-terminated k,v list) ---------------- */

static void
kvdev_rados_free_config(char **config)
{
	char **entry;

	if (config) {
		for (entry = config; *entry; entry++) {
			free(*entry);
		}
		free(config);
	}
}

static char **
kvdev_rados_dup_config(const char *const *config)
{
	size_t count;
	char **copy;

	if (!config) {
		return NULL;
	}
	for (count = 0; config[count]; count++) {}
	copy = calloc(count + 1, sizeof(*copy));
	if (!copy) {
		return NULL;
	}
	for (count = 0; config[count]; count++) {
		if (!(copy[count] = strdup(config[count]))) {
			kvdev_rados_free_config(copy);
			return NULL;
		}
	}
	return copy;
}

/* ---- key -> oid hex encoding --------------------------------------------- */

/*
 * Encode a binary key (1-16 bytes) into a NUL-terminated lowercase-hex oid.
 * librados oids are C strings, so a binary key cannot be used verbatim; hex is
 * collision-free and reversible. oid must hold KVDEV_RADOS_OID_MAX bytes.
 */
static void
kvdev_rados_key_to_oid(const void *key, uint8_t key_len, char *oid)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *k = key;
	uint8_t i;

	for (i = 0; i < key_len; i++) {
		oid[i * 2]     = hex[k[i] >> 4];
		oid[i * 2 + 1] = hex[k[i] & 0xf];
	}
	oid[key_len * 2] = '\0';
}

/* ---- cluster registry ---------------------------------------------------- */

static void
kvdev_rados_cluster_free(struct kvdev_rados_cluster *entry)
{
	if (entry == NULL) {
		return;
	}
	kvdev_rados_free_config(entry->config_param);
	free(entry->config_file);
	free(entry->key_file);
	free(entry->user_id);
	free(entry->name);
	free(entry);
}

/* Look up a cluster by name and take a reference. Caller holds no lock. */
static int
kvdev_rados_get_cluster(const char *cluster_name, rados_t **cluster)
{
	struct kvdev_rados_cluster *entry;

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (strcmp(cluster_name, entry->name) == 0) {
			entry->ref++;
			*cluster = &entry->cluster;
			pthread_mutex_unlock(&g_clusters_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);
	return -ENODEV;
}

static void
kvdev_rados_put_cluster(rados_t **cluster)
{
	struct kvdev_rados_cluster *entry;

	if (cluster == NULL || *cluster == NULL) {
		return;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (*cluster == &entry->cluster) {
			assert(entry->ref > 0);
			entry->ref--;
			*cluster = NULL;
			/* On shutdown the registry is being torn down: when the last
			 * referencing kvdev releases the cluster, shut it down here so
			 * module_fini's sweep (which already ran) need not chase the
			 * async kvdev frees. */
			if (g_shutting_down && entry->ref == 0) {
				STAILQ_REMOVE(&g_clusters, entry, kvdev_rados_cluster, link);
				rados_shutdown(entry->cluster);
				pthread_mutex_unlock(&g_clusters_mutex);
				kvdev_rados_cluster_free(entry);
				return;
			}
			pthread_mutex_unlock(&g_clusters_mutex);
			return;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);
	SPDK_ERRLOG("Cannot find registry entry for cluster=%p\n", (void *)cluster);
}

/*
 * Create + connect a rados handle: rados_create(user_id) -> conf_read_file /
 * conf_set -> connect. Runs on a non-SPDK thread (via spdk_call_unaffinitized)
 * so it does not contend with reactors. Connect CAN fail; on any failure the
 * partially-built entry is fully torn down (no leak).
 */
static int
kvdev_rados_cluster_connect(struct kvdev_rados_cluster *entry)
{
	int rc;

	rc = rados_create(&entry->cluster, entry->user_id);
	if (rc < 0) {
		SPDK_ERRLOG("rados_create failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	/* Read ceph.conf: explicit path must succeed; default path is best-effort. */
	rc = rados_conf_read_file(entry->cluster, entry->config_file);
	if (entry->config_file && rc < 0) {
		SPDK_ERRLOG("Failed to read conf file %s\n", entry->config_file);
		goto err;
	}

	if (entry->config_param) {
		char **e = entry->config_param;
		while (*e) {
			rc = rados_conf_set(entry->cluster, e[0], e[1]);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to set %s = %s\n", e[0], e[1]);
				goto err;
			}
			e += 2;
		}
	}

	if (entry->key_file) {
		rc = rados_conf_set(entry->cluster, "keyring", entry->key_file);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to set keyring = %s\n", entry->key_file);
			goto err;
		}
	}

	rc = rados_connect(entry->cluster);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to connect rados cluster '%s': %s\n",
			    entry->name, spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	rados_shutdown(entry->cluster);
	entry->cluster = NULL;
	return rc;
}

static int
kvdev_rados_do_register_cluster(const struct kvdev_rados_cluster_info *info)
{
	struct kvdev_rados_cluster *entry;
	int rc;

	if (info == NULL || info->name == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (strcmp(info->name, entry->name) == 0) {
			pthread_mutex_unlock(&g_clusters_mutex);
			SPDK_ERRLOG("Cluster name '%s' already exists\n", info->name);
			return -EEXIST;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return -ENOMEM;
	}

	entry->name = strdup(info->name);
	if (entry->name == NULL) {
		rc = -ENOMEM;
		goto err;
	}
	if (info->user_id) {
		entry->user_id = strdup(info->user_id);
		if (entry->user_id == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (info->config_param) {
		entry->config_param = kvdev_rados_dup_config(info->config_param);
		if (entry->config_param == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (info->config_file) {
		entry->config_file = strdup(info->config_file);
		if (entry->config_file == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (info->key_file) {
		entry->key_file = strdup(info->key_file);
		if (entry->key_file == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}

	rc = kvdev_rados_cluster_connect(entry);
	if (rc < 0) {
		goto err;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_INSERT_TAIL(&g_clusters, entry, link);
	pthread_mutex_unlock(&g_clusters_mutex);
	SPDK_NOTICELOG("Registered rados cluster '%s'\n", entry->name);
	return 0;

err:
	kvdev_rados_cluster_free(entry);
	return rc;
}

struct kvdev_rados_register_ctx {
	const struct kvdev_rados_cluster_info	*info;
	int					rc;
};

static void *
kvdev_rados_register_cluster_unaff(void *arg)
{
	struct kvdev_rados_register_ctx *ctx = arg;

	ctx->rc = kvdev_rados_do_register_cluster(ctx->info);
	return arg;
}

int
kvdev_rados_register_cluster(const struct kvdev_rados_cluster_info *info)
{
	struct kvdev_rados_register_ctx ctx = { .info = info, .rc = -1 };

	/* rados_connect spins up threads; run it off the SPDK reactor. */
	spdk_call_unaffinitized(kvdev_rados_register_cluster_unaff, &ctx);
	return ctx.rc;
}

int
kvdev_rados_unregister_cluster(const char *name)
{
	struct kvdev_rados_cluster *entry;

	if (name == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (strcmp(name, entry->name) == 0) {
			if (entry->ref != 0) {
				pthread_mutex_unlock(&g_clusters_mutex);
				SPDK_ERRLOG("Cluster '%s' still in use (ref=%u)\n",
					    name, entry->ref);
				return -EBUSY;
			}
			STAILQ_REMOVE(&g_clusters, entry, kvdev_rados_cluster, link);
			rados_shutdown(entry->cluster);
			pthread_mutex_unlock(&g_clusters_mutex);
			kvdev_rados_cluster_free(entry);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);
	return -ENODEV;
}

static void
kvdev_rados_dump_cluster(struct kvdev_rados_cluster *entry, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "cluster_name", entry->name);
	if (entry->user_id) {
		spdk_json_write_named_string(w, "user_id", entry->user_id);
	}
	if (entry->config_param) {
		char **e = entry->config_param;
		spdk_json_write_named_object_begin(w, "config_param");
		while (*e) {
			spdk_json_write_named_string(w, e[0], e[1]);
			e += 2;
		}
		spdk_json_write_object_end(w);
	}
	if (entry->config_file) {
		spdk_json_write_named_string(w, "config_file", entry->config_file);
	}
	if (entry->key_file) {
		spdk_json_write_named_string(w, "key_file", entry->key_file);
	}
	spdk_json_write_object_end(w);
}

int
kvdev_rados_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name)
{
	struct kvdev_rados_cluster *entry;
	struct spdk_json_write_ctx *w;

	pthread_mutex_lock(&g_clusters_mutex);

	if (name) {
		STAILQ_FOREACH(entry, &g_clusters, link) {
			if (strcmp(name, entry->name) == 0) {
				w = spdk_jsonrpc_begin_result(request);
				kvdev_rados_dump_cluster(entry, w);
				spdk_jsonrpc_end_result(request, w);
				pthread_mutex_unlock(&g_clusters_mutex);
				return 0;
			}
		}
		pthread_mutex_unlock(&g_clusters_mutex);
		return -ENOENT;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		kvdev_rados_dump_cluster(entry, w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	pthread_mutex_unlock(&g_clusters_mutex);
	return 0;
}

/* ---- IO path ------------------------------------------------------------- */

/* Translate a librados return value into a kvdev status for a given op. */
static int
kvdev_rados_xlate_status(enum kvdev_rados_op op, int ret)
{
	if (ret >= 0) {
		return SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	switch (ret) {
	case -ENOENT:
		return SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST;
	case -EEXIST:
		return SPDK_KVDEV_IO_STATUS_KEY_EXIST;
	case -ENOSPC:
	case -EDQUOT:
		return SPDK_KVDEV_IO_STATUS_NOMEM;
	case -EOPNOTSUPP:
	case -ENOSYS:
		/* KV Exec: the OSD has no such object class, or the class has no
		 * such method (rados returns -EOPNOTSUPP for an unregistered
		 * class/method). Surface NOT_SUPPORTED so the NVMf layer maps it to
		 * an NVMe not-supported/invalid-opcode status rather than a generic
		 * failure. (-ENOSYS is treated the same for older OSDs.) */
		if (op == KVDEV_RADOS_OP_EXEC) {
			return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
		}
		return SPDK_KVDEV_IO_STATUS_FAILED;
	default:
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
}

/* Finish one harvested IO: derive status/value_len, fire the cb, free state. */
static void
kvdev_rados_io_finish(struct kvdev_rados_io *io)
{
	int ret = rados_aio_get_return_value(io->comp);
	int status = kvdev_rados_xlate_status(io->op, ret);
	uint32_t value_len = 0;

	if (io->op == KVDEV_RADOS_OP_RETRIEVE) {
		/* The read_op bundles read + stat in one round-trip. ret is the
		 * operate() return; the per-sub-op read result is io->read_rval and
		 * the TRUE object size is io->stat_size. Report the true length so
		 * the host can detect truncation and resize (matches kvdev_mem). */
		status = kvdev_rados_xlate_status(io->op, ret);
		if (ret >= 0 && io->read_rval >= 0) {
			uint64_t true_size = io->stat_size;

			value_len = (uint32_t)spdk_min(true_size, KVDEV_RADOS_MAX_VALUE_LEN);
			if (true_size > io->buf_len) {
				/* read_op copied at most buf_len bytes; report the
				 * full length so the host knows it was truncated. */
				status = SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL;
			} else {
				status = SPDK_KVDEV_IO_STATUS_SUCCESS;
			}
		} else if (ret >= 0) {
			/* operate() succeeded but the read sub-op failed. */
			status = kvdev_rados_xlate_status(io->op, io->read_rval);
		}
	} else if (io->op == KVDEV_RADOS_OP_EXIST && ret >= 0) {
		status = SPDK_KVDEV_IO_STATUS_SUCCESS;
	} else if (io->op == KVDEV_RADOS_OP_EXEC && ret >= 0) {
		/*
		 * rados_aio_exec returns the number of output bytes the cls method
		 * produced and copied into the caller buffer. Unlike Retrieve (which
		 * pairs a stat sub-op to learn the TRUE length), rados gives no way to
		 * recover the full output length when the cls output exceeds the host
		 * buffer: librados copies at most out_len bytes and the return value is
		 * that copied count. So we report the bytes returned as value_len and
		 * flag BUFFER_TOO_SMALL only when the output exactly filled the buffer
		 * (a possible truncation). The host's buffer should be sized to the
		 * advertised op output; this is the documented rados-backend contract. */
		value_len = (uint32_t)ret;
		if ((uint32_t)ret >= io->buf_len && io->buf_len > 0) {
			status = SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL;
		} else {
			status = SPDK_KVDEV_IO_STATUS_SUCCESS;
		}
	}

	io->cb_fn(io->cb_arg, status, value_len);

	rados_aio_release(io->comp);
	if (io->write_op) {
		rados_release_write_op(io->write_op);
	}
	if (io->read_op) {
		rados_release_read_op(io->read_op);
	}
	free(io);
}

/*
 * Poller: harvest completed librados aios on the SPDK thread. librados fires its
 * own callbacks from internal threads, so completion delivery to the kvdev
 * consumer happens here, never in a librados thread.
 */
static int
kvdev_rados_poll(void *arg)
{
	struct kvdev_rados_io_channel *ch = arg;
	struct kvdev_rados_io *io, *tmp;
	int count = 0;

	TAILQ_FOREACH_SAFE(io, &ch->inflight, link, tmp) {
		if (!rados_aio_is_complete(io->comp)) {
			continue;
		}
		TAILQ_REMOVE(&ch->inflight, io, link);
		kvdev_rados_io_finish(io);
		count++;
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/* librados completion callback. Intentionally a no-op: the poller harvests on
 * the SPDK thread. A callback is required by rados_aio_create_completion. */
static void
kvdev_rados_aio_cb(rados_completion_t comp, void *arg)
{
}

static struct kvdev_rados_io *
kvdev_rados_io_alloc(struct kvdev_rados_io_channel *ch, enum kvdev_rados_op op,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io *io;

	io = calloc(1, sizeof(*io));
	if (io == NULL) {
		return NULL;
	}
	io->op = op;
	io->cb_fn = cb_fn;
	io->cb_arg = cb_arg;

	if (rados_aio_create_completion((void *)io, kvdev_rados_aio_cb, NULL,
					&io->comp) < 0) {
		free(io);
		return NULL;
	}
	return io;
}

static int
kvdev_rados_store(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		  const void *value, uint32_t value_len,
		  const struct spdk_kvdev_store_opts *opts,
		  spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	uint32_t flags = SPDK_KVDEV_STORE_FLAG_NONE;
	uint32_t ttl = 0;
	bool ttl_valid = false;
	int rc;

	if (value_len > rdev->kvdev.caps.max_value_len) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	/* Honour opts->size when reading extensible fields. */
	if (opts && opts->size >= offsetof(struct spdk_kvdev_store_opts, flags) +
	    sizeof(opts->flags)) {
		flags = opts->flags;
	}
	if ((flags & SPDK_KVDEV_STORE_F_TTL) && opts &&
	    opts->size >= offsetof(struct spdk_kvdev_store_opts, ttl) + sizeof(opts->ttl)) {
		ttl = opts->ttl;
		ttl_valid = true;
	}

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_STORE, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	/*
	 * Build an atomic write op so SIKE/SINKE and the value write happen as
	 * one rados operation:
	 *   SINKE -> create(EXCLUSIVE): fails -EEXIST if the object exists.
	 *   SIKE  -> assert_exists():   fails -ENOENT if the object is absent.
	 * write_full replaces the whole object (object-per-KV).
	 */
	io->write_op = rados_create_write_op();
	if (io->write_op == NULL) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	if (flags & SPDK_KVDEV_STORE_FLAG_SINKE) {
		rados_write_op_create(io->write_op, LIBRADOS_CREATE_EXCLUSIVE, NULL);
	} else if (flags & SPDK_KVDEV_STORE_FLAG_SIKE) {
		rados_write_op_assert_exists(io->write_op);
	}

	rados_write_op_write_full(io->write_op, value, value_len);

	if (ttl_valid) {
		/* Store-only TTL persisted as an xattr (ADR-0003 spirit). Never
		 * enforced: no lazy expiry / reaper. */
		char ttlbuf[16];
		int n = snprintf(ttlbuf, sizeof(ttlbuf), "%u", ttl);
		rados_write_op_setxattr(io->write_op, KVDEV_RADOS_TTL_XATTR, ttlbuf, n);
	}

	rc = rados_aio_write_op_operate(io->write_op, rdev->io_ctx, io->comp, oid,
					NULL, 0);
	if (rc < 0) {
		rados_release_write_op(io->write_op);
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_STORE, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_retrieve(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		     void *value_buf, uint32_t buf_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	int rc;

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_RETRIEVE, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	io->buf_len = buf_len;

	/*
	 * Bundle read + stat in a single aio read_op so we learn the TRUE object
	 * size in the same round-trip: read copies min(size, buf_len) bytes, stat
	 * yields the full size. This lets Retrieve report the true value length
	 * (and BUFFER_TOO_SMALL when truncated), matching the in-memory module,
	 * without a second network round-trip.
	 */
	io->read_op = rados_create_read_op();
	if (io->read_op == NULL) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rados_read_op_read(io->read_op, 0, buf_len, value_buf, &io->bytes_read,
			   &io->read_rval);
	rados_read_op_stat(io->read_op, &io->stat_size, &io->stat_mtime, NULL);

	rc = rados_aio_read_op_operate(io->read_op, rdev->io_ctx, io->comp, oid, 0);
	if (rc < 0) {
		rados_release_read_op(io->read_op);
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_RETRIEVE, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_op_delete(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	int rc;

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_DELETE, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rc = rados_aio_remove(rdev->io_ctx, oid, io->comp);
	if (rc < 0) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_DELETE, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_exist(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		  spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	int rc;

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_EXIST, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rc = rados_aio_stat(rdev->io_ctx, oid, io->comp, &io->stat_size, &io->stat_mtime);
	if (rc < 0) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_EXIST, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

/*
 * KV Exec (ADR-0005): map an allowlisted op to a rados object-class method.
 *
 * The (class, method) is NOT on the data path: the NVMf layer resolves the
 * data-plane op_id against the per-namespace allowlist (KVX-2) and passes the
 * matching entry's opaque binding through to us. We parse the binding as
 * "class:method" (the format this backend defines): everything before the first
 * ':' is the rados object-class name, everything after is the method. A binding
 * that is NULL, empty, missing the ':', or with an empty class/method is
 * rejected INVALID before any rados call. op_id itself is not interpreted here
 * (it only selected the binding upstream).
 *
 * The cls runs server-side on the OSD owning oid = hex(key) via rados_aio_exec;
 * the harvest/poller pattern is identical to the other ops (we never touch SPDK
 * state from a librados thread). The method's output is bounded by the host
 * buffer (output_buf_len); see kvdev_rados_io_finish for the truncation contract.
 */
static int
kvdev_rados_exec(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		 uint32_t op_id, const char *binding,
		 const void *input, uint32_t input_len,
		 void *output_buf, uint32_t output_buf_len,
		 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	char cls[256];
	const char *colon, *method;
	size_t cls_len;
	int rc;

	(void)op_id;

	/* Parse the binding "class:method". Reject anything malformed. */
	if (binding == NULL || (colon = strchr(binding, ':')) == NULL) {
		SPDK_ERRLOG("KV Exec: binding '%s' is not 'class:method'\n",
			    binding ? binding : "(null)");
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}
	cls_len = (size_t)(colon - binding);
	method = colon + 1;
	if (cls_len == 0 || cls_len >= sizeof(cls) || method[0] == '\0') {
		SPDK_ERRLOG("KV Exec: binding '%s' has empty/oversized class or method\n",
			    binding);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}
	memcpy(cls, binding, cls_len);
	cls[cls_len] = '\0';

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_EXEC, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	io->buf_len = output_buf_len;

	/*
	 * rados_aio_exec runs cls::method on the OSD owning oid, feeding it the
	 * input blob and copying up to output_buf_len bytes of its output into
	 * output_buf. The completion return value carries the output byte count
	 * (or a negative errno: -ENOENT object missing -> KEY_NOT_EXIST,
	 * -EOPNOTSUPP unknown class/method -> NOT_SUPPORTED; see xlate_status).
	 */
	rc = rados_aio_exec(rdev->io_ctx, oid, io->comp, cls, method,
			    (const char *)input, input_len,
			    (char *)output_buf, output_buf_len);
	if (rc < 0) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_EXEC, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

/*
 * List is deferred for the rados backend (ADR-0002): rados object enumeration is
 * an unordered cursor that cannot seek to a spec start-key, so spec-conformant
 * paginated List is out of scope for this slice. Report command-not-supported.
 */
static int
kvdev_rados_list(struct spdk_io_channel *_ch, const void *start_key, uint8_t start_key_len,
		 spdk_kvdev_list_cb iter_cb, void *iter_arg,
		 spdk_kvdev_list_done_cb done_cb, void *done_arg)
{
	done_cb(done_arg, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0);
	return 0;
}

/* ---- channel / lifecycle ------------------------------------------------- */

static int
kvdev_rados_create_channel_cb(void *io_device, void *ctx_buf)
{
	struct kvdev_rados_io_channel *ch = ctx_buf;

	ch->rdev = io_device;
	TAILQ_INIT(&ch->inflight);
	/* 0 period -> poll every reactor tick; aios complete out of band. */
	ch->poller = SPDK_POLLER_REGISTER(kvdev_rados_poll, ch, 0);
	if (ch->poller == NULL) {
		return -ENOMEM;
	}
	return 0;
}

static void
kvdev_rados_destroy_channel_cb(void *io_device, void *ctx_buf)
{
	struct kvdev_rados_io_channel *ch = ctx_buf;

	/* Drain any still-inflight aios so we neither leak nor complete after
	 * the channel is gone. */
	while (!TAILQ_EMPTY(&ch->inflight)) {
		struct kvdev_rados_io *io = TAILQ_FIRST(&ch->inflight);

		rados_aio_wait_for_complete(io->comp);
		TAILQ_REMOVE(&ch->inflight, io, link);
		kvdev_rados_io_finish(io);
	}
	spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *
kvdev_rados_get_io_channel(void *ctx)
{
	struct kvdev_rados *rdev = ctx;

	return spdk_get_io_channel(rdev);
}

static void
kvdev_rados_free(struct kvdev_rados *rdev)
{
	if (rdev == NULL) {
		return;
	}
	if (rdev->io_ctx) {
		rados_ioctx_destroy(rdev->io_ctx);
	}
	kvdev_rados_put_cluster(&rdev->cluster_p);
	free(rdev->cluster_name);
	free(rdev->pool_name);
	free(rdev->namespace_name);
	free(rdev->kvdev.name);
	free(rdev);
}

static void
kvdev_rados_unregister_io_device_done(void *io_device)
{
	kvdev_rados_free(io_device);
}

static int
kvdev_rados_destruct(void *ctx)
{
	struct kvdev_rados *rdev = ctx;

	TAILQ_REMOVE(&g_kvdevs, rdev, tailq);
	spdk_io_device_unregister(rdev, kvdev_rados_unregister_io_device_done);
	return 0;
}

static const struct spdk_kvdev_fn_table kvdev_rados_fn_table = {
	.destruct	= kvdev_rados_destruct,
	.get_io_channel	= kvdev_rados_get_io_channel,
	.store		= kvdev_rados_store,
	.retrieve	= kvdev_rados_retrieve,
	.del		= kvdev_rados_op_delete,
	.exist		= kvdev_rados_exist,
	.list		= kvdev_rados_list,
	.exec		= kvdev_rados_exec,
};

/*
 * Create the per-instance ioctx on the pool (== subsystem) and set the rados
 * namespace (== KV namespace) on it. Runs unaffinitized to mirror bdev_rbd's
 * ioctx creation. Pools are operator pre-provisioned (ADR-0004): ioctx_create
 * fails if the pool is absent — we surface that rather than auto-create.
 */
static void *
kvdev_rados_init_ioctx(void *arg)
{
	struct kvdev_rados *rdev = arg;
	int rc;

	rc = rados_ioctx_create(*rdev->cluster_p, rdev->pool_name, &rdev->io_ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create ioctx on pool '%s': %s\n",
			    rdev->pool_name, spdk_strerror(-rc));
		rdev->io_ctx = NULL;
		return NULL;
	}

	if (rdev->namespace_name) {
		rados_ioctx_set_namespace(rdev->io_ctx, rdev->namespace_name);
	}

	return arg;
}

int
kvdev_rados_create(const struct kvdev_rados_opts *opts, struct spdk_kvdev **_kvdev)
{
	struct kvdev_rados *rdev;
	uint32_t max_value_len;
	int rc;

	if (opts == NULL || opts->name == NULL || opts->cluster_name == NULL ||
	    opts->pool_name == NULL) {
		return -EINVAL;
	}

	max_value_len = opts->max_value_len ? opts->max_value_len : KVDEV_RADOS_MAX_VALUE_LEN;

	rdev = calloc(1, sizeof(*rdev));
	if (rdev == NULL) {
		return -ENOMEM;
	}

	rdev->kvdev.name = strdup(opts->name);
	rdev->cluster_name = strdup(opts->cluster_name);
	rdev->pool_name = strdup(opts->pool_name);
	if (rdev->kvdev.name == NULL || rdev->cluster_name == NULL ||
	    rdev->pool_name == NULL) {
		rc = -ENOMEM;
		goto err;
	}
	if (opts->namespace_name) {
		rdev->namespace_name = strdup(opts->namespace_name);
		if (rdev->namespace_name == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}

	rc = kvdev_rados_get_cluster(rdev->cluster_name, &rdev->cluster_p);
	if (rc < 0) {
		SPDK_ERRLOG("Unknown rados cluster '%s' (register it first)\n",
			    rdev->cluster_name);
		goto err;
	}

	/* ioctx creation/connect can fail (missing pool, etc.); clean up fully. */
	if (spdk_call_unaffinitized(kvdev_rados_init_ioctx, rdev) == NULL) {
		rc = -ENODEV;
		goto err;
	}

	rdev->kvdev.ctxt = rdev;
	rdev->kvdev.fn_table = &kvdev_rados_fn_table;
	rdev->kvdev.module = &g_kvdev_rados_module;
	rdev->kvdev.caps.max_key_len = SPDK_KVDEV_KEY_MAX_LEN;
	rdev->kvdev.caps.max_value_len = max_value_len;
	rdev->kvdev.caps.max_num_keys = 0;	/* unbounded; rados scales out */
	if (!spdk_uuid_is_null(&opts->uuid)) {
		spdk_uuid_copy(&rdev->kvdev.uuid, &opts->uuid);
	}

	spdk_io_device_register(rdev, kvdev_rados_create_channel_cb,
				kvdev_rados_destroy_channel_cb,
				sizeof(struct kvdev_rados_io_channel), opts->name);

	rc = spdk_kvdev_register(&rdev->kvdev);
	if (rc != 0) {
		spdk_io_device_unregister(rdev, NULL);
		goto err;
	}

	TAILQ_INSERT_TAIL(&g_kvdevs, rdev, tailq);

	*_kvdev = &rdev->kvdev;
	SPDK_NOTICELOG("Created rados kvdev '%s' (pool=%s, namespace=%s, max_value_len=%u)\n",
		       opts->name, opts->pool_name,
		       opts->namespace_name ? opts->namespace_name : "(default)",
		       max_value_len);
	return 0;

err:
	kvdev_rados_free(rdev);
	return rc;
}

int
kvdev_rados_delete(const char *name)
{
	struct spdk_kvdev *kvdev;

	kvdev = spdk_kvdev_get_by_name(name);
	if (kvdev == NULL) {
		return -ENODEV;
	}
	if (kvdev->module != &g_kvdev_rados_module) {
		SPDK_ERRLOG("kvdev '%s' is not a rados kvdev\n", name);
		return -EINVAL;
	}
	return spdk_kvdev_unregister(kvdev);
}

/*
 * module_init clears g_shutting_down so a finish-then-reinit of the kvdev
 * subsystem in the same process starts clean: module_fini latches it true, and
 * without this reset a subsequent re-init could shut a cluster down out from
 * under a live kvdev when a ref drops to 0.
 */
static int
kvdev_rados_module_init(void)
{
	g_shutting_down = false;
	return 0;
}

static void
kvdev_rados_module_fini(void)
{
	struct kvdev_rados *rdev, *tmp;
	struct kvdev_rados_cluster *c, *ctmp;

	g_shutting_down = true;

	/* Deleting a kvdev frees it asynchronously (io_device unregister), and the
	 * async free releases the cluster ref. With g_shutting_down set, the last
	 * release shuts the cluster down (see kvdev_rados_put_cluster). */
	TAILQ_FOREACH_SAFE(rdev, &g_kvdevs, tailq, tmp) {
		kvdev_rados_delete(rdev->kvdev.name);
	}

	/* Sweep any clusters never referenced by a kvdev (ref == 0). Referenced
	 * ones are shut down by the async put_cluster above. */
	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH_SAFE(c, &g_clusters, link, ctmp) {
		if (c->ref != 0) {
			continue;
		}
		STAILQ_REMOVE(&g_clusters, c, kvdev_rados_cluster, link);
		rados_shutdown(c->cluster);
		kvdev_rados_cluster_free(c);
	}
	pthread_mutex_unlock(&g_clusters_mutex);
}

void
kvdev_rados_write_config_json(struct spdk_json_write_ctx *w)
{
	struct kvdev_rados_cluster *c;
	struct kvdev_rados *rdev;

	/* Clusters first: a kvdev_rados_create references one by name. */
	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(c, &g_clusters, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "kvdev_rados_register_cluster");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", c->name);
		if (c->user_id) {
			spdk_json_write_named_string(w, "user_id", c->user_id);
		}
		if (c->config_file) {
			spdk_json_write_named_string(w, "config_file", c->config_file);
		}
		if (c->key_file) {
			spdk_json_write_named_string(w, "key_file", c->key_file);
		}
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	pthread_mutex_unlock(&g_clusters_mutex);

	TAILQ_FOREACH(rdev, &g_kvdevs, tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "kvdev_rados_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", rdev->kvdev.name);
		spdk_json_write_named_string(w, "cluster_name", rdev->cluster_name);
		spdk_json_write_named_string(w, "pool_name", rdev->pool_name);
		if (rdev->namespace_name) {
			spdk_json_write_named_string(w, "namespace", rdev->namespace_name);
		}
		spdk_json_write_named_uint32(w, "max_value_len", rdev->kvdev.caps.max_value_len);
		if (!spdk_uuid_is_null(&rdev->kvdev.uuid)) {
			spdk_json_write_named_uuid(w, "uuid", &rdev->kvdev.uuid);
		}
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT(kvdev_rados)
