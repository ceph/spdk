/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_RBD_H
#define SPDK_BDEV_RBD_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/rpc.h"

struct cluster_register_info {
	char *name;
	char *user_id;
	char **config_param;
	char *config_file;
	char *key_file;
	char *core_mask;
};

void dump_cluster_nonce(struct spdk_json_write_ctx *w, const char *name);
void bdev_rbd_free_config(char **config);
char **bdev_rbd_dup_config(const char *const *config);
void bdev_rbd_free_passphrase(char **passphrase);

typedef void (*spdk_delete_rbd_complete)(void *cb_arg, int bdeverrno);

int bdev_rbd_create(struct spdk_bdev **bdev, const char *name, const char *user_id,
		    const char *pool_name, const char *namespace_name,
		    const char *const *config,
		    const char *rbd_name, uint32_t block_size, const char *cluster_name,
		    const struct spdk_uuid *uuid, bool read_only, bool fail_io,
		    uint32_t size_delta, uint32_t encryption_entries_count,
		    const uint32_t *encryption_format, const char **passphrase);
/**
 * Delete rbd bdev.
 *
 * \param name Name of rbd bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_rbd_delete(const char *name, spdk_delete_rbd_complete cb_fn,
		     void *cb_arg);

/**
 * Resize rbd bdev.
 *
 * \param bdev Name of rbd bdev.
 * \param new_size_in_mb The new size in MiB for this bdev.
 */
int bdev_rbd_resize(const char *name, const uint64_t new_size_in_mb);

/**
 * Create a Rados cluster.
 *
 * \param info the info to register the Rados cluster object
 */
int bdev_rbd_register_cluster(struct cluster_register_info *info);

/**
 * Delete a registered cluster.
 *
 * \param name the name of the cluster to be deleted.
 */
int bdev_rbd_unregister_cluster(const char *name);

/**
 * Show the cluster info of a given name. If given name is empty,
 * the info of every registered cluster name will be showed.
 *
 * \param request the json request.
 * \param name the name of the cluster.
 */
int bdev_rbd_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name);

/**
 * Wait for latest osd map on cluster identified by name.
 * Part of blocklist logic
 *
 * \param name the name of the cluster.
 */
int bdev_rbd_wait_for_latest_osdmap(const char *name);

/**
 * Get the current rbd_with_crc32c setting.
 *
 * \return true if CRC32C optimization is enabled, false otherwise
 */
bool bdev_rbd_get_with_crc32c(void);

/**
 * Set the rbd_with_crc32c parameter to enable/disable CRC32C optimization
 * for RBD write operations.
 *
 * \param enable true to enable CRC32C optimization, false to disable
 */
void bdev_rbd_set_with_crc32c(bool enable);

/** Keep the current value of a slow/stuck I/O threshold in bdev_rbd_set_diag(). */
#define BDEV_RBD_DIAG_KEEP	UINT64_MAX

typedef void (*bdev_rbd_set_diag_cb)(void *cb_arg, int rc);

/**
 * Get the slow/stuck I/O diagnostics settings.
 *
 * \param enable set to whether diagnostics are on.
 * \param slow_io_ms set to the slow I/O log threshold in ms (0 = off).
 * \param stuck_io_ms set to the stuck I/O watchdog threshold in ms (0 = off).
 */
void bdev_rbd_get_diag(bool *enable, uint64_t *slow_io_ms, uint64_t *stuck_io_ms);

/**
 * Turn slow/stuck I/O diagnostics on or off at runtime, on every SPDK thread.
 * Must be called from the app thread after the bdev layer is initialized.
 *
 * \param enable true to turn diagnostics on.
 * \param slow_io_ms slow I/O log threshold in ms (0 = off), or BDEV_RBD_DIAG_KEEP.
 * \param stuck_io_ms stuck I/O watchdog threshold in ms (0 = off), or BDEV_RBD_DIAG_KEEP.
 * \param cb_fn called once every thread has the new settings.
 * \param cb_arg argument for cb_fn.
 *
 * \return 0 if started (cb_fn will be called), -EBUSY if a change is in progress.
 */
int bdev_rbd_set_diag(bool enable, uint64_t slow_io_ms, uint64_t stuck_io_ms,
		      bdev_rbd_set_diag_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_RBD_H */
