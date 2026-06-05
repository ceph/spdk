/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * NVMe driver public API extension for Key-Value Command Set
 */

#ifndef SPDK_NVME_KV_H
#define SPDK_NVME_KV_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum length of a key in bytes. */
#define SPDK_NVME_KV_KEY_MIN_LEN 1

/** Maximum length of a key in bytes (per the NVMe Key-Value Command Set Specification). */
#define SPDK_NVME_KV_KEY_MAX_LEN 16

/**
 * Get the Key-Value Command Set Specific Identify Namespace data
 * as defined by the NVMe Key-Value Command Set Specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the namespace data, or NULL if the namespace is not
 * a Key-Value namespace.
 */
const struct spdk_nvme_kv_ns_data *spdk_nvme_kv_ns_get_data(const struct spdk_nvme_ns *ns);

/**
 * Get the Key-Value Command Set Specific Identify Controller data
 * as defined by the NVMe Key-Value Command Set Specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return pointer to the controller data, or NULL if the controller does not
 * support the Key-Value Command Set.
 */
const struct spdk_nvme_kv_ctrlr_data *
spdk_nvme_kv_ctrlr_get_data(const struct spdk_nvme_ctrlr *ctrlr);

/**
 * Submit a KV Store command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the KV Store command.
 * \param qpair I/O queue pair to submit the request.
 * \param key Pointer to the key buffer.
 * \param key_len Length of the key in bytes (SPDK_NVME_KV_KEY_MIN_LEN to SPDK_NVME_KV_KEY_MAX_LEN).
 * \param value Pointer to the value buffer. May be NULL if value_len is 0.
 * \param value_len Length of the value in bytes. 0 stores an empty value.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param options Store options, see spdk_nvme_kv_store_option in nvme_spec.h.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_kv_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       const void *key, uint8_t key_len,
		       const void *value, uint32_t value_len,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint8_t options);

/**
 * Size-versioned options for spdk_nvme_kv_store_ext().
 *
 * Vendor extension (ADR-0003). The struct is versioned by its leading \c size
 * field so later additions stay ABI-compatible: callers set \c size to
 * sizeof(struct spdk_nvme_kv_store_ext_opts) and the driver only reads members
 * that fall within the supplied size. Populate via
 * spdk_nvme_kv_store_ext_opts_init() before setting fields.
 */
struct spdk_nvme_kv_store_ext_opts {
	/** Size of this structure as known to the caller. Must be set first. */
	size_t		size;
	/**
	 * Time-to-live in seconds. When non-zero, the driver sets CDW12 to this
	 * value and the TTL Valid Store Option bit
	 * (SPDK_NVME_KV_STORE_OPT_TTL_VALID) in CDW11. The target persists the
	 * TTL but does NOT enforce expiry (store-only). A zero TTL means "no
	 * TTL" and leaves CDW12 / the TTL Valid bit clear.
	 */
	uint32_t	ttl;
};

/**
 * Initialize a KV Store ext options struct to defaults. \c size is typically
 * sizeof(struct spdk_nvme_kv_store_ext_opts).
 */
static inline void
spdk_nvme_kv_store_ext_opts_init(struct spdk_nvme_kv_store_ext_opts *opts, size_t size)
{
	memset(opts, 0, size);
	opts->size = size;
}

/**
 * Submit a KV Store command with vendor extension options (ADR-0003).
 *
 * Behaves exactly like spdk_nvme_kv_store() but additionally carries a
 * size-versioned opts struct. When opts->ttl is non-zero the command sets the
 * TTL Valid Store Option bit and CDW12 = opts->ttl (seconds); the target
 * persists the TTL without enforcing expiry.
 *
 * \param ns NVMe namespace to submit the KV Store command.
 * \param qpair I/O queue pair to submit the request.
 * \param key Pointer to the key buffer.
 * \param key_len Length of the key in bytes (SPDK_NVME_KV_KEY_MIN_LEN to SPDK_NVME_KV_KEY_MAX_LEN).
 * \param value Pointer to the value buffer.
 * \param value_len Length of the value in bytes.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param options Store options, see spdk_nvme_kv_store_option in nvme_spec.h.
 * \param opts Size-versioned extension options (may be NULL for none).
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_kv_store_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   const void *key, uint8_t key_len,
			   const void *value, uint32_t value_len,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			   uint8_t options,
			   const struct spdk_nvme_kv_store_ext_opts *opts);

/**
 * Submit a KV Retrieve command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the KV Retrieve command.
 * \param qpair I/O queue pair to submit the request.
 * \param key Pointer to the key buffer.
 * \param key_len Length of the key in bytes (SPDK_NVME_KV_KEY_MIN_LEN to SPDK_NVME_KV_KEY_MAX_LEN).
 * \param value Pointer to the value buffer to store the retrieved value. May be
 * NULL if value_len is 0.
 * \param value_len Length of the value buffer in bytes. 0 transfers no data and
 * only reports the value size in CDW0 of the completion.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param options Retrieve options, see spdk_nvme_kv_retrieve_option in nvme_spec.h.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_kv_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  const void *key, uint8_t key_len,
			  void *value, uint32_t value_len,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			  uint8_t options);

/**
 * Submit a KV Delete command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the KV Delete command.
 * \param qpair I/O queue pair to submit the request.
 * \param key Pointer to the key buffer.
 * \param key_len Length of the key in bytes (SPDK_NVME_KV_KEY_MIN_LEN to SPDK_NVME_KV_KEY_MAX_LEN).
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_kv_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			const void *key, uint8_t key_len,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a KV Exist command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the KV Exist command.
 * \param qpair I/O queue pair to submit the request.
 * \param key Pointer to the key buffer.
 * \param key_len Length of the key in bytes (SPDK_NVME_KV_KEY_MIN_LEN to SPDK_NVME_KV_KEY_MAX_LEN).
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_kv_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       const void *key, uint8_t key_len,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a KV List command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the KV List command.
 * \param qpair I/O queue pair to submit the request.
 * \param start_key Pointer to the starting key for iteration (may be NULL for all keys).
 * \param start_key_len Length of the starting key in bytes (0-16).
 * \param buffer Pointer to the buffer to store the list of keys.
 * \param buffer_len Length of the buffer in bytes.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_kv_list(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      const void *start_key, uint8_t start_key_len,
		      void *buffer, uint32_t buffer_len,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif
