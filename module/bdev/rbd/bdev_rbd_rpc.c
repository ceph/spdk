/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_rbd.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

static int
bdev_rbd_decode_config(const struct spdk_json_val *values, void *out)
{
	char ***map = out;
	char **entry;
	uint32_t i;

	if (values->type == SPDK_JSON_VAL_NULL) {
		/* treated like empty object: empty config */
		*map = calloc(1, sizeof(**map));
		if (!*map) {
			return -1;
		}
		return 0;
	}

	if (values->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -1;
	}

	*map = calloc(values->len + 1, sizeof(**map));
	if (!*map) {
		return -1;
	}

	for (i = 0, entry = *map; i < values->len;) {
		const struct spdk_json_val *name = &values[i + 1];
		const struct spdk_json_val *v = &values[i + 2];
		/* Here we catch errors like invalid types. */
		if (!(entry[0] = spdk_json_strdup(name)) ||
		    !(entry[1] = spdk_json_strdup(v))) {
			bdev_rbd_free_config(*map);
			*map = NULL;
			return -1;
		}
		i += 1 + spdk_json_val_len(v);
		entry += 2;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_rbd_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_create_ctx, name), spdk_json_decode_string, true},
	{"user_id", offsetof(struct rpc_bdev_rbd_create_ctx, user_id), spdk_json_decode_string, true},
	{"pool_name", offsetof(struct rpc_bdev_rbd_create_ctx, pool_name), spdk_json_decode_string},
	{"rbd_name", offsetof(struct rpc_bdev_rbd_create_ctx, rbd_name), spdk_json_decode_string},
	{"namespace_name", offsetof(struct rpc_bdev_rbd_create_ctx, namespace_name), spdk_json_decode_string, true},
	{"block_size", offsetof(struct rpc_bdev_rbd_create_ctx, block_size), spdk_json_decode_uint32},
	{"config", offsetof(struct rpc_bdev_rbd_create_ctx, config), bdev_rbd_decode_config, true},
	{"cluster_name", offsetof(struct rpc_bdev_rbd_create_ctx, cluster_name), spdk_json_decode_string, true},
	{"uuid", offsetof(struct rpc_bdev_rbd_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"read_only", offsetof(struct rpc_bdev_rbd_create_ctx, read_only), spdk_json_decode_bool, true},
	{"encryption_format", offsetof(struct rpc_bdev_rbd_create_ctx, encryption_format), rpc_decode_encryption_format, true},
	{"passphrase", offsetof(struct rpc_bdev_rbd_create_ctx, passphrase), rpc_decode_passphrase, true},
	{"fail_io", offsetof(struct rpc_bdev_rbd_create_ctx, fail_io), spdk_json_decode_bool, true},
	{"size_delta", offsetof(struct rpc_bdev_rbd_create_ctx, size_delta), spdk_json_decode_uint32, true}
};

static void
rpc_bdev_rbd_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_create_ctx req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_rbd, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.passphrase.count != req.encryption_format.count) {
		SPDK_DEBUGLOG(bdev_rbd, "passphrase count (%lu) must be equal to format count (%lu)\n", req.passphrase.count, req.encryption_format.count);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "count mismatch");
		goto cleanup;
	}
	rc = bdev_rbd_create(&bdev, req.name, req.user_id, req.pool_name, req.namespace_name,
			     (const char *const *)req.config,
			     req.rbd_name,
			     req.block_size, req.cluster_name, &req.uuid, req.read_only, req.fail_io,
			     req.size_delta, req.passphrase.count, req.encryption_format.items,
			     (const char **)req.passphrase.items);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_rbd_create(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_create", rpc_bdev_rbd_create, SPDK_RPC_RUNTIME)

/**
 * RPC function to get the current rbd_with_crc32c setting
 */
static void
rpc_bdev_rbd_get_with_crc32c(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, bdev_rbd_get_with_crc32c());
	spdk_jsonrpc_end_result(request, w);
}

/**
 * RPC function to set the rbd_with_crc32c parameter
 */

static const struct spdk_json_object_decoder rpc_bdev_rbd_set_with_crc32c_decoders[] = {
	{"enable", offsetof(struct rpc_bdev_rbd_set_with_crc32c_ctx, enable), spdk_json_decode_bool},
};

static void
rpc_bdev_rbd_set_with_crc32c(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_set_with_crc32c_ctx req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_set_with_crc32c_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_set_with_crc32c_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						"Missing or invalid enable parameter");
		return;
	}

	bdev_rbd_set_with_crc32c(req.enable);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, bdev_rbd_get_with_crc32c());
	spdk_jsonrpc_end_result(request, w);
}

static const struct spdk_json_object_decoder rpc_bdev_rbd_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_delete_ctx, name), spdk_json_decode_string},
};

static void
_rpc_bdev_rbd_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_rbd_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_rbd_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_rbd_delete(req.name, _rpc_bdev_rbd_delete_cb, request);

cleanup:
	free_rpc_bdev_rbd_delete(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_delete", rpc_bdev_rbd_delete, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_rbd_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_resize_ctx, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_rbd_resize_ctx, new_size), spdk_json_decode_uint64}
};

static void
rpc_bdev_rbd_resize(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_resize_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_resize_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_resize(req.name, req.new_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_rbd_resize(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_resize", rpc_bdev_rbd_resize, SPDK_RPC_RUNTIME)

/* TODO: replace with free_rpc_bdev_rbd_register_cluster */
static void
free_rpc_bdev_rbd_register_cluster_ctx(struct cluster_register_info *req)
{
	free(req->name);
	free(req->user_id);
	bdev_rbd_free_config(req->config_param);
	free(req->config_file);
	free(req->key_file);
	free(req->core_mask);
}

static const struct spdk_json_object_decoder rpc_bdev_rbd_register_cluster_decoders[] = {
	{"name", offsetof(struct cluster_register_info, name), spdk_json_decode_string},
	{"user_id", offsetof(struct cluster_register_info, user_id), spdk_json_decode_string, true},
	{"config_param", offsetof(struct cluster_register_info, config_param), bdev_rbd_decode_config, true},
	{"config_file", offsetof(struct cluster_register_info, config_file), spdk_json_decode_string, true},
	{"key_file", offsetof(struct cluster_register_info, key_file), spdk_json_decode_string, true},
	{"core_mask", offsetof(struct cluster_register_info, core_mask), spdk_json_decode_string, true}
};

static void
rpc_bdev_rbd_register_cluster(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct cluster_register_info req = {};
	int rc = 0;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_register_cluster_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_register_cluster_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_rbd, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_register_cluster(&req);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}
	w = spdk_jsonrpc_begin_result(request);
	dump_cluster_nonce(w, req.name);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_rpc_bdev_rbd_register_cluster_ctx(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_register_cluster", rpc_bdev_rbd_register_cluster, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_rbd_unregister_cluster_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_unregister_cluster_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_rbd_unregister_cluster(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_unregister_cluster_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_unregister_cluster_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_unregister_cluster_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_unregister_cluster(req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_rbd_unregister_cluster(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_unregister_cluster", rpc_bdev_rbd_unregister_cluster, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_rbd_get_clusters_info_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_get_clusters_info_ctx, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_rbd_get_clusters_info(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_get_clusters_info_ctx req = {};
	int rc;

	if (params && spdk_json_decode_object(params, rpc_bdev_rbd_get_clusters_info_decoders,
					      SPDK_COUNTOF(rpc_bdev_rbd_get_clusters_info_decoders),
					      &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_get_clusters_info(request, req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	free_rpc_bdev_rbd_get_clusters_info(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_get_clusters_info", rpc_bdev_rbd_get_clusters_info, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_rbd_wait_for_latest_osdmap_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_wait_for_latest_osdmap_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_rbd_wait_for_latest_osdmap(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_wait_for_latest_osdmap_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_wait_for_latest_osdmap_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_wait_for_latest_osdmap_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_wait_for_latest_osdmap(req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_rbd_wait_for_latest_osdmap(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_wait_for_latest_osdmap", rpc_bdev_rbd_wait_for_latest_osdmap, SPDK_RPC_RUNTIME)

SPDK_RPC_REGISTER("bdev_rbd_get_with_crc32c", rpc_bdev_rbd_get_with_crc32c, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER("bdev_rbd_set_with_crc32c", rpc_bdev_rbd_set_with_crc32c, SPDK_RPC_STARTUP)

/* Upper bound for the diagnostics thresholds (1 day), so ms * ticks_hz can't overflow. */
#define RPC_BDEV_RBD_DIAG_MAX_MS	(24ULL * 60 * 60 * 1000)

static void
rpc_bdev_rbd_write_diag(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w;
	bool enable;
	uint64_t slow_io_ms, stuck_io_ms;

	bdev_rbd_get_diag(&enable, &slow_io_ms, &stuck_io_ms);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_bool(w, "enable", enable);
	spdk_json_write_named_uint64(w, "slow_io_ms", slow_io_ms);
	spdk_json_write_named_uint64(w, "stuck_io_ms", stuck_io_ms);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_rbd_get_diag(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_rbd_get_diag requires no parameters");
		return;
	}
	rpc_bdev_rbd_write_diag(request);
}
SPDK_RPC_REGISTER("bdev_rbd_get_diag", rpc_bdev_rbd_get_diag, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_rbd_set_diag_decoders[] = {
	{"enable", offsetof(struct rpc_bdev_rbd_set_diag_ctx, enable), spdk_json_decode_bool},
	{"slow_io_ms", offsetof(struct rpc_bdev_rbd_set_diag_ctx, slow_io_ms), spdk_json_decode_uint64, true},
	{"stuck_io_ms", offsetof(struct rpc_bdev_rbd_set_diag_ctx, stuck_io_ms), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_rbd_set_diag_done(void *cb_arg, int rc)
{
	struct rpc_bdev_rbd_set_diag_ctx *req = cb_arg;
	struct spdk_jsonrpc_request *request = req->request;

	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	rpc_bdev_rbd_write_diag(request);
}

/*
 * Turn slow/stuck I/O diagnostics on or off without restarting. Thresholds
 * that aren't given keep their current values. Returns the new settings once
 * every SPDK thread has them.
 */
static void
rpc_bdev_rbd_set_diag(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_set_diag_ctx *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	req->request = request;
	req->slow_io_ms = BDEV_RBD_DIAG_KEEP;
	req->stuck_io_ms = BDEV_RBD_DIAG_KEEP;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_set_diag_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_set_diag_decoders), req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}
	if ((req->slow_io_ms != BDEV_RBD_DIAG_KEEP && req->slow_io_ms > RPC_BDEV_RBD_DIAG_MAX_MS) ||
	    (req->stuck_io_ms != BDEV_RBD_DIAG_KEEP && req->stuck_io_ms > RPC_BDEV_RBD_DIAG_MAX_MS)) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "slow_io_ms and stuck_io_ms must be <= %llu",
						     RPC_BDEV_RBD_DIAG_MAX_MS);
		free_rpc_bdev_rbd_set_diag(req);
		free(req);
		return;
	}

	rc = bdev_rbd_set_diag(req->enable, req->slow_io_ms, req->stuck_io_ms,
			       rpc_bdev_rbd_set_diag_done, req);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}

	free_rpc_bdev_rbd_set_diag(req);
	free(req);
}
SPDK_RPC_REGISTER("bdev_rbd_set_diag", rpc_bdev_rbd_set_diag, SPDK_RPC_RUNTIME)
