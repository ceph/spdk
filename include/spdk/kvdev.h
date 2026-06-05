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
	 */
	int (*store)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		     const void *value, uint32_t value_len,
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

	/* TODO (later slices): delete, exist, list slot in here. */
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
 * Submit a Store on the descriptor's kvdev. Thin wrappers over the fn_table.
 */
int spdk_kvdev_store(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		     const void *key, uint8_t key_len,
		     const void *value, uint32_t value_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Retrieve on the descriptor's kvdev.
 */
int spdk_kvdev_retrieve(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
			const void *key, uint8_t key_len,
			void *value_buf, uint32_t buf_len,
			spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KVDEV_H */
