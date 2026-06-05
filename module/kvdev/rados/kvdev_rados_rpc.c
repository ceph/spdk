/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"

#include "kvdev_rados.h"
#include "spdk_internal/rpc_autogen.h"

static void
free_rpc_kvdev_rados_register_cluster_ctx(struct rpc_kvdev_rados_register_cluster_ctx *r)
{
	char **e;

	free(r->name);
	free(r->user_id);
	free(r->config_file);
	free(r->key_file);
	if (r->config_param) {
		for (e = (char **)(r->config_param); *e; e++) {
			free(*e);
		}
		free(r->config_param);
	}
}

/* Decode a {"k":"v", ...} object into a NULL-terminated k,v,k,v,... array. */
static int
kvdev_rados_decode_config(const struct spdk_json_val *values, void *out)
{
	char ***map = out;
	char **entry;
	uint32_t i;

	if (values->type == SPDK_JSON_VAL_NULL) {
		*map = calloc(1, sizeof(**map));
		return *map ? 0 : -1;
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

		if (!(entry[0] = spdk_json_strdup(name)) ||
		    !(entry[1] = spdk_json_strdup(v))) {
			goto err;
		}
		i += 1 + spdk_json_val_len(v);
		entry += 2;
	}
	return 0;

err:
	for (entry = *map; *entry; entry++) {
		free(*entry);
	}
	free(*map);
	*map = NULL;
	return -1;
}

static const struct spdk_json_object_decoder rpc_kvdev_rados_register_cluster_decoders_manual[] = {
	{"name", offsetof(struct rpc_kvdev_rados_register_cluster_ctx, name), spdk_json_decode_string},
	{"user_id", offsetof(struct rpc_kvdev_rados_register_cluster_ctx, user_id), spdk_json_decode_string, true},
	{"config_param", offsetof(struct rpc_kvdev_rados_register_cluster_ctx, config_param), kvdev_rados_decode_config, true},
	{"config_file", offsetof(struct rpc_kvdev_rados_register_cluster_ctx, config_file), spdk_json_decode_string, true},
	{"key_file", offsetof(struct rpc_kvdev_rados_register_cluster_ctx, key_file), spdk_json_decode_string, true},
};

static void
rpc_kvdev_rados_register_cluster(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_kvdev_rados_register_cluster_ctx req = {};
	struct kvdev_rados_cluster_info info = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_rados_register_cluster_decoders_manual,
				    SPDK_COUNTOF(rpc_kvdev_rados_register_cluster_decoders_manual), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	info.name = req.name;
	info.user_id = req.user_id;
	info.config_param = (const char *const *)req.config_param;
	info.config_file = req.config_file;
	info.key_file = req.key_file;

	rc = kvdev_rados_register_cluster(&info);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_kvdev_rados_register_cluster_ctx(&req);
}
SPDK_RPC_REGISTER("kvdev_rados_register_cluster", rpc_kvdev_rados_register_cluster,
		  SPDK_RPC_RUNTIME)

static void
rpc_kvdev_rados_unregister_cluster(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_kvdev_rados_unregister_cluster_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_rados_unregister_cluster_decoders,
				    SPDK_COUNTOF(rpc_kvdev_rados_unregister_cluster_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = kvdev_rados_unregister_cluster(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_kvdev_rados_unregister_cluster(&req);
}
SPDK_RPC_REGISTER("kvdev_rados_unregister_cluster", rpc_kvdev_rados_unregister_cluster,
		  SPDK_RPC_RUNTIME)

static void
rpc_kvdev_rados_get_clusters(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_kvdev_rados_get_clusters_ctx req = {};
	int rc;

	if (params != NULL &&
	    spdk_json_decode_object(params, rpc_kvdev_rados_get_clusters_decoders,
				    SPDK_COUNTOF(rpc_kvdev_rados_get_clusters_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = kvdev_rados_get_clusters_info(request, req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}

cleanup:
	free_rpc_kvdev_rados_get_clusters(&req);
}
SPDK_RPC_REGISTER("kvdev_rados_get_clusters", rpc_kvdev_rados_get_clusters, SPDK_RPC_RUNTIME)

static void
rpc_kvdev_rados_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_kvdev_rados_create_ctx req = {};
	struct kvdev_rados_opts opts = {};
	struct spdk_kvdev *kvdev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_rados_create_decoders,
				    SPDK_COUNTOF(rpc_kvdev_rados_create_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.name = req.name;
	opts.cluster_name = req.cluster_name;
	opts.pool_name = req.pool_name;
	opts.namespace_name = req.namespace;
	opts.uuid = req.uuid;
	opts.max_value_len = req.max_value_len;

	rc = kvdev_rados_create(&opts, &kvdev);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_kvdev_get_name(kvdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_kvdev_rados_create(&req);
}
SPDK_RPC_REGISTER("kvdev_rados_create", rpc_kvdev_rados_create, SPDK_RPC_RUNTIME)

static void
rpc_kvdev_rados_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_kvdev_rados_delete_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_rados_delete_decoders,
				    SPDK_COUNTOF(rpc_kvdev_rados_delete_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = kvdev_rados_delete(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_kvdev_rados_delete(&req);
}
SPDK_RPC_REGISTER("kvdev_rados_delete", rpc_kvdev_rados_delete, SPDK_RPC_RUNTIME)
