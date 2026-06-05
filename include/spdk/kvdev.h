/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * Key-Value Device (kvdev) public interface.
 *
 * kvdev is an object-shaped storage abstraction that is a sibling to the
 * block device (bdev) layer.  Where bdev models a linear array of fixed-size
 * blocks, kvdev models a mapping from a short binary key (1-16 bytes) to a
 * variable-length value.  It mirrors bdev's *runtime model* (module
 * registration via a constructor macro, descriptors for open/close, per-thread
 * spdk_io_channels and async completions) while exposing an object-shaped op
 * set.
 */

#ifndef SPDK_KVDEV_H
#define SPDK_KVDEV_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum length of a key in bytes. */
#define SPDK_KVDEV_KEY_MIN_LEN 1

/** Maximum length of a key in bytes (per the NVMe Key-Value Command Set Specification). */
#define SPDK_KVDEV_KEY_MAX_LEN 16

struct spdk_kvdev;
struct spdk_kvdev_desc;
struct spdk_kvdev_module;

/**
 * Status codes returned via kvdev completions.  These are deliberately a small
 * device-neutral set; the NVMf layer translates them into NVMe status codes.
 */
enum spdk_kvdev_io_status {
	SPDK_KVDEV_IO_STATUS_SUCCESS		= 0,
	/** Generic failure. */
	SPDK_KVDEV_IO_STATUS_FAILED		= -1,
	/** The requested key does not exist. */
	SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST	= -2,
	/** The key already exists (e.g. a Store-If-No-Key-Exists conflict). */
	SPDK_KVDEV_IO_STATUS_KEY_EXIST		= -6,
	/** The provided buffer was too small to hold the value. */
	SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL	= -3,
	/** The request was malformed (bad key length, etc.). */
	SPDK_KVDEV_IO_STATUS_INVALID		= -4,
	/** Out of memory / capacity. */
	SPDK_KVDEV_IO_STATUS_NOMEM		= -5,
};

/**
 * kvdev I/O completion callback.
 *
 * \param cb_arg Context passed to the originating op.
 * \param status One of enum spdk_kvdev_io_status.
 * \param value_len For retrieve, the actual length of the stored value (which
 *                  may exceed the supplied buffer; see status). Ignored for
 *                  store.
 */
typedef void (*spdk_kvdev_io_completion_cb)(void *cb_arg, int status, uint32_t value_len);

/**
 * kvdev List iteration callback. Invoked once per key the backend visits, in
 * the backend's stable iteration order, until the consumer asks it to stop.
 *
 * \param cb_arg Context passed to the originating list op.
 * \param key Key bytes for this entry (valid only for the duration of the call).
 * \param key_len Length of \c key in bytes (in [1,16]).
 *
 * \return true to continue iterating to the next key, false to stop early (for
 *         example, when the next key would not fit in the host buffer).
 */
typedef bool (*spdk_kvdev_list_cb)(void *cb_arg, const void *key, uint8_t key_len);

/**
 * kvdev List completion callback. Fired once after iteration finishes (either
 * the backend ran out of keys or the per-key callback asked it to stop).
 *
 * \param cb_arg Context passed to the originating list op.
 * \param status One of enum spdk_kvdev_io_status (SUCCESS unless the request
 *               could not be serviced).
 * \param num_keys Number of keys the per-key callback accepted (i.e. the count
 *                 for which spdk_kvdev_list_cb returned and the key was emitted).
 */
typedef void (*spdk_kvdev_list_done_cb)(void *cb_arg, int status, uint32_t num_keys);

/**
 * Store conditional flags. These mirror the NVMe KV Store command "Store
 * Option" bits (CDW11 bits 8/9 in the SQE) so the NVMf layer can pass the
 * host's intent straight through to the backend.
 */
enum spdk_kvdev_store_flags {
	/** Default behaviour: insert or overwrite unconditionally. */
	SPDK_KVDEV_STORE_FLAG_NONE	= 0,
	/**
	 * Store-If-Key-Exists (SIKE). Only store when the key already exists;
	 * otherwise fail with SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST.
	 */
	SPDK_KVDEV_STORE_FLAG_SIKE	= 1u << 0,
	/**
	 * Store-If-No-Key-Exists (SINKE). Only store when the key does not yet
	 * exist; otherwise fail with SPDK_KVDEV_IO_STATUS_KEY_EXIST.
	 */
	SPDK_KVDEV_STORE_FLAG_SINKE	= 1u << 1,
	/**
	 * TTL valid (vendor extension, ADR-0003). When set, opts->ttl carries a
	 * time-to-live in seconds that the backend persists alongside the value.
	 * Store-only: the TTL is recorded but never enforced (no lazy expiry, no
	 * reaper). When clear, opts->ttl is ignored.
	 */
	SPDK_KVDEV_STORE_F_TTL		= 1u << 2,
};

/**
 * Extensible options for a Store operation.
 *
 * The struct is versioned by its leading \c size field: callers set \c size to
 * sizeof(struct spdk_kvdev_store_opts) and backends must only read fields that
 * fall within the supplied size. This lets later slices append members without
 * breaking the store ABI. Always populate via spdk_kvdev_store_opts_init()
 * before setting fields.
 */
struct spdk_kvdev_store_opts {
	/** Size of this structure as known to the caller. Must be set first. */
	size_t		size;
	/** Bitmask of enum spdk_kvdev_store_flags. */
	uint32_t	flags;

	/*
	 * Time-to-live in seconds (vendor extension, ADR-0003). Honoured only
	 * when SPDK_KVDEV_STORE_F_TTL is set in \c flags; otherwise ignored.
	 * Store-only: the backend persists this as a deadline but does not
	 * enforce expiry. Future slices APPEND fields below this line (never
	 * reorder/remove existing ones); the \c size field handles versioning.
	 */
	uint32_t	ttl;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_kvdev_store_opts) == 16, "Incorrect size");

/**
 * Initialize a store options struct to defaults. \c size is typically
 * sizeof(struct spdk_kvdev_store_opts).
 */
static inline void
spdk_kvdev_store_opts_init(struct spdk_kvdev_store_opts *opts, size_t size)
{
	memset(opts, 0, size);
	opts->size = size;
}

/**
 * Advertised capabilities/limits of a kvdev. Used by the NVMf layer to build
 * KV Identify namespace data (kvkml/kvvml/mnks).
 */
struct spdk_kvdev_caps {
	/** Maximum key length in bytes (<= SPDK_KVDEV_KEY_MAX_LEN). */
	uint16_t	max_key_len;
	/** Maximum value length in bytes. */
	uint32_t	max_value_len;
	/** Maximum number of keys (0 = no limit advertised). */
	uint32_t	max_num_keys;
};

/**
 * The function table that a kvdev module supplies for each kvdev it creates.
 *
 * All I/O ops are asynchronous: they queue/perform the work and invoke
 * cb_fn(cb_arg, status, value_len) on the calling thread's io_channel context.
 * Ops return 0 if the request was accepted (the completion will fire) or a
 * negative errno if it could not even be submitted (no completion fires).
 *
 * The vtable is intentionally op-per-verb so later slices (delete/exist/list)
 * slot in as additional members without disturbing the store/retrieve ABI.
 */
struct spdk_kvdev_fn_table {
	/** Tear down the kvdev. Called from spdk_kvdev_unregister context. */
	int (*destruct)(void *ctx);

	/** Return a per-thread io_channel for this kvdev. */
	struct spdk_io_channel *(*get_io_channel)(void *ctx);

	/**
	 * Store (insert or overwrite) a value under a key.
	 *
	 * \param ch io_channel obtained from get_io_channel().
	 * \param key Key bytes (key_len in [1,16]).
	 * \param value Value bytes to store (value_len bytes).
	 * \param opts Store options (flags such as SIKE/SINKE, future TTL). May be
	 *             NULL for default (unconditional) behaviour. The backend must
	 *             honour opts->size and ignore fields beyond it.
	 */
	int (*store)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		     const void *value, uint32_t value_len,
		     const struct spdk_kvdev_store_opts *opts,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Retrieve the value stored under a key into value_buf.
	 *
	 * On completion, value_len reports the true stored length. If the
	 * buffer was too small the status is SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
	 * and buf_len bytes are still copied.
	 */
	int (*retrieve)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
			void *value_buf, uint32_t buf_len,
			spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Delete the key (and its value) from the kvdev.
	 *
	 * On completion the status is SPDK_KVDEV_IO_STATUS_SUCCESS if the key was
	 * present and removed, or SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if it was
	 * absent. The value_len argument to the completion is unused.
	 */
	int (*del)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Test whether the key exists in the kvdev.
	 *
	 * On completion the status is SPDK_KVDEV_IO_STATUS_SUCCESS if the key is
	 * present, or SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if it is absent. The
	 * value_len argument to the completion is unused.
	 */
	int (*exist)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * List (iterate) the keys present in the kvdev.
	 *
	 * The backend visits keys in an UNSPECIFIED but STABLE order: the order is
	 * not defined, but as long as the namespace is not modified (no store/delete)
	 * two List calls visit keys in the same sequence. \c start_key is a POSITION
	 * into that stable order: iteration begins at start_key if it is present,
	 * otherwise at the first key that sorts at-or-after start_key in the backend's
	 * order (a vendor-specific-but-stable start point per the KV spec §2.1.6.2).
	 * A NULL/zero-length start_key begins at the very first key.
	 *
	 * For each visited key the backend invokes \c iter_cb(iter_arg, key, key_len)
	 * and stops early as soon as it returns false. When iteration finishes the
	 * backend invokes \c done_cb(done_arg, status, num_keys) where num_keys is the
	 * number of keys the iterator callback accepted.
	 *
	 * \param ch io_channel obtained from get_io_channel().
	 * \param start_key Position key, or NULL to start from the beginning.
	 * \param start_key_len Length of start_key (0 when start_key is NULL).
	 */
	int (*list)(struct spdk_io_channel *ch, const void *start_key, uint8_t start_key_len,
		    spdk_kvdev_list_cb iter_cb, void *iter_arg,
		    spdk_kvdev_list_done_cb done_cb, void *done_arg);

	/**
	 * Execute a server-side operation against a key (vendor extension,
	 * ADR-0005, "KV Exec"). OPTIONAL: a module may leave this NULL, in which
	 * case spdk_kvdev_exec() returns -ENOTSUP and the NVMf layer maps the
	 * command to an NVMe not-supported status. This keeps backends that do
	 * not (yet) support exec valid (e.g. the librados module, KVX-3).
	 *
	 * The operation is selected by a small integer \c op_id (never a
	 * class/method string on the data path). \c input/input_len carry the
	 * input blob; the backend writes its output into \c output_buf (at most
	 * \c output_buf_len bytes). On completion the value_len argument reports
	 * the TRUE output length: if it exceeds \c output_buf_len the status is
	 * SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL and output_buf_len bytes are
	 * still copied (same truncation contract as Retrieve). An unknown op_id
	 * fails with SPDK_KVDEV_IO_STATUS_INVALID.
	 *
	 * \param ch io_channel obtained from get_io_channel().
	 * \param key Key bytes (key_len in [1,16]).
	 * \param op_id Operation identifier selecting the server-side operation.
	 * \param input Input blob bytes (may be NULL when input_len is 0).
	 * \param input_len Length of \c input in bytes.
	 * \param output_buf Buffer that receives the output blob.
	 * \param output_buf_len Capacity of \c output_buf in bytes.
	 */
	int (*exec)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		    uint32_t op_id, const void *input, uint32_t input_len,
		    void *output_buf, uint32_t output_buf_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);
};

/**
 * A registered kvdev instance.  A module embeds this in its own private struct
 * and fills in name/ctxt/fn_table/module/caps before calling
 * spdk_kvdev_register().
 */
struct spdk_kvdev {
	/** Unique name of this kvdev. */
	char					*name;

	/** Module-private context passed back to the fn_table ops. */
	void					*ctxt;

	/** Operation table supplied by the module. */
	const struct spdk_kvdev_fn_table	*fn_table;

	/** Owning module. */
	struct spdk_kvdev_module		*module;

	/** Advertised limits. */
	struct spdk_kvdev_caps			caps;

	/** Unique identifier. */
	struct spdk_uuid			uuid;

	/* Internal kvdev-layer bookkeeping below this line. */
	TAILQ_HEAD(, spdk_kvdev_desc)		open_descs;
	TAILQ_ENTRY(spdk_kvdev)			internal_link;
};

/**
 * A kvdev module.  Registered once at load time via SPDK_KVDEV_MODULE_REGISTER.
 */
struct spdk_kvdev_module {
	/** Module name. */
	const char	*name;

	/** Optional module init / fini hooks. */
	int		(*module_init)(void);
	void		(*module_fini)(void);

	TAILQ_ENTRY(spdk_kvdev_module)	internal_link;
};

/**
 * Register a kvdev module. Use the SPDK_KVDEV_MODULE_REGISTER macro instead of
 * calling this directly.
 */
void spdk_kvdev_module_list_add(struct spdk_kvdev_module *module);

#define SPDK_KVDEV_MODULE_REGISTER(_name, _module)					\
	static void __attribute__((constructor)) _spdk_kvdev_module_register_##_name(void) \
	{										\
		spdk_kvdev_module_list_add(_module);					\
	}

/**
 * Initialize the kvdev subsystem. Runs registered module_init hooks. Safe to
 * call multiple times (idempotent).
 */
int spdk_kvdev_initialize(void);

/**
 * Tear down the kvdev subsystem and run module_fini hooks.
 */
void spdk_kvdev_finish(void);

/**
 * Register a kvdev. The module fills in the spdk_kvdev fields first.
 *
 * \return 0 on success, negative errno on failure (e.g. -EEXIST on name clash).
 */
int spdk_kvdev_register(struct spdk_kvdev *kvdev);

/**
 * Unregister a kvdev by name. Fails if descriptors are still open.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_kvdev_unregister(struct spdk_kvdev *kvdev);

/**
 * Look up a kvdev by name.
 *
 * \return the kvdev, or NULL if not found.
 */
struct spdk_kvdev *spdk_kvdev_get_by_name(const char *name);

/**
 * Iterate registered kvdevs.
 */
struct spdk_kvdev *spdk_kvdev_first(void);
struct spdk_kvdev *spdk_kvdev_next(struct spdk_kvdev *prev);

/**
 * Open a kvdev by name, obtaining a descriptor.
 *
 * \param name kvdev name.
 * \param write Reserved for future use (write access). Currently informational.
 * \param desc Output descriptor handle.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_kvdev_open(const char *name, bool write, struct spdk_kvdev_desc **desc);

/**
 * Close a previously opened descriptor.
 */
void spdk_kvdev_close(struct spdk_kvdev_desc *desc);

/**
 * Get the kvdev backing a descriptor.
 */
struct spdk_kvdev *spdk_kvdev_desc_get_kvdev(struct spdk_kvdev_desc *desc);

/**
 * Get the kvdev name.
 */
const char *spdk_kvdev_get_name(const struct spdk_kvdev *kvdev);

/**
 * Get the advertised capabilities of a kvdev.
 */
const struct spdk_kvdev_caps *spdk_kvdev_get_caps(const struct spdk_kvdev *kvdev);

/**
 * Get a per-thread io_channel for the kvdev backing this descriptor.
 *
 * \return io_channel, or NULL on failure. Release with spdk_put_io_channel().
 */
struct spdk_io_channel *spdk_kvdev_get_io_channel(struct spdk_kvdev_desc *desc);

/**
 * Submit a Store on the descriptor's kvdev. Thin wrapper over the fn_table.
 *
 * \param opts Store options (SIKE/SINKE flags, future TTL). May be NULL for
 *             default (unconditional insert-or-overwrite) behaviour.
 */
int spdk_kvdev_store(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		     const void *key, uint8_t key_len,
		     const void *value, uint32_t value_len,
		     const struct spdk_kvdev_store_opts *opts,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Retrieve on the descriptor's kvdev.
 */
int spdk_kvdev_retrieve(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
			const void *key, uint8_t key_len,
			void *value_buf, uint32_t buf_len,
			spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Delete on the descriptor's kvdev. Thin wrapper over the fn_table.
 *
 * The completion fires with SPDK_KVDEV_IO_STATUS_SUCCESS if the key existed and
 * was deleted, or SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if the key was absent. The
 * completion's value_len argument is unused.
 */
int spdk_kvdev_delete(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		      const void *key, uint8_t key_len,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit an Exist on the descriptor's kvdev. Thin wrapper over the fn_table.
 *
 * The completion fires with SPDK_KVDEV_IO_STATUS_SUCCESS if the key exists, or
 * SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if it does not. The completion's value_len
 * argument is unused.
 */
int spdk_kvdev_exist(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		     const void *key, uint8_t key_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a List (key iteration) on the descriptor's kvdev. Thin wrapper over
 * the fn_table.
 *
 * Keys are visited in the backend's unspecified-but-stable order; \c start_key
 * is a position into that order (NULL/0 starts from the beginning). \c iter_cb
 * is invoked per key and returns false to stop early; \c done_cb fires once at
 * the end with the accepted key count. See struct spdk_kvdev_fn_table::list.
 *
 * \param start_key Position key, or NULL to start from the first key.
 * \param start_key_len Length of start_key in bytes; must be 0 when start_key
 *                      is NULL.
 *
 * \return 0 if the request was accepted (a completion will fire), negative
 *         errno if it could not be submitted (no completion fires).
 */
int spdk_kvdev_list(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		    const void *start_key, uint8_t start_key_len,
		    spdk_kvdev_list_cb iter_cb, void *iter_arg,
		    spdk_kvdev_list_done_cb done_cb, void *done_arg);

/**
 * Submit a KV Exec (vendor extension, ADR-0005) on the descriptor's kvdev. Thin
 * wrapper over the fn_table's optional \c exec op.
 *
 * Runs the server-side operation selected by \c op_id against \c key, passing
 * the \c input blob and scattering the operation's output (bounded by
 * \c output_buf_len) into \c output_buf. The completion's value_len argument
 * reports the true output length; if it exceeds \c output_buf_len the status is
 * SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL (output_buf_len bytes still copied).
 *
 * \return 0 if the request was accepted (a completion will fire), -ENOTSUP if
 *         the backend has no exec op (no completion fires), or another negative
 *         errno if it could not be submitted.
 */
int spdk_kvdev_exec(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		    const void *key, uint8_t key_len, uint32_t op_id,
		    const void *input, uint32_t input_len,
		    void *output_buf, uint32_t output_buf_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KVDEV_H */
