/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk_internal/rpc_autogen.h"

#include "kvdev_mem.h"

static void
rpc_kvdev_mem_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_kvdev_mem_create_ctx req = {};
	struct kvdev_mem_opts opts = {};
	struct spdk_kvdev *kvdev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_mem_create_decoders,
				    SPDK_COUNTOF(rpc_kvdev_mem_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(kvdev_mem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.name = req.name;
	opts.uuid = req.uuid;
	opts.max_value_len = req.max_value_len;
	opts.max_num_keys = req.max_num_keys;

	rc = kvdev_mem_create(&opts, &kvdev);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_kvdev_get_name(kvdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_kvdev_mem_create(&req);
}
SPDK_RPC_REGISTER("kvdev_mem_create", rpc_kvdev_mem_create, SPDK_RPC_RUNTIME)

static void
rpc_kvdev_mem_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_kvdev_mem_delete_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_mem_delete_decoders,
				    SPDK_COUNTOF(rpc_kvdev_mem_delete_decoders),
				    &req)) {
		SPDK_DEBUGLOG(kvdev_mem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = kvdev_mem_delete(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_kvdev_mem_delete(&req);
}
SPDK_RPC_REGISTER("kvdev_mem_delete", rpc_kvdev_mem_delete, SPDK_RPC_RUNTIME)

/*
 * Debug/diagnostic RPC: inspect a stored entry, notably the store-only vendor
 * TTL (ADR-0003), so end-to-end tests can assert the TTL round-trips into the
 * backend. The key is passed as a UTF-8/ASCII string (1-16 bytes).
 */

static void
rpc_kvdev_mem_get_entry(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_kvdev_mem_get_entry_ctx req = {};
	struct kvdev_mem_entry_info info = {};
	struct spdk_json_write_ctx *w;
	size_t key_len;
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_mem_get_entry_decoders,
				    SPDK_COUNTOF(rpc_kvdev_mem_get_entry_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	key_len = strlen(req.key);
	if (key_len < 1 || key_len > 16) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "key must be 1-16 bytes");
		goto cleanup;
	}

	rc = kvdev_mem_get_entry(req.name, req.key, (uint8_t)key_len, &info);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "value_len", info.value_len);
	spdk_json_write_named_bool(w, "ttl_valid", info.ttl_valid);
	spdk_json_write_named_uint32(w, "ttl", info.ttl);
	spdk_json_write_named_uint64(w, "deadline", info.deadline);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_kvdev_mem_get_entry(&req);
}
SPDK_RPC_REGISTER("kvdev_mem_get_entry", rpc_kvdev_mem_get_entry, SPDK_RPC_RUNTIME)
