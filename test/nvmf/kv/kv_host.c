/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * Minimal KV host (initiator) for the end-to-end vfio-user round-trip test.
 *
 * It attaches to an NVMf controller over the VFIOUSER transport, finds the
 * first Key-Value namespace (CSI == SPDK_NVME_CSI_KV), issues a KV Store and
 * then a KV Retrieve for the same key, and verifies the retrieved bytes match.
 *
 * Usage: kv_host <vfio-user-socket-dir>
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"
#include "spdk/string.h"

static const char g_key[] = "kvkey01";
static const char g_value[] = "the quick brown fox jumps over the lazy dog";

struct kv_ctx {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	volatile bool		done;
	volatile bool		failed;
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct kv_ctx *ctx = cb_ctx;

	ctx->ctrlr = ctrlr;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct kv_ctx *ctx = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "KV command failed: sct=%d sc=%d\n",
			cpl->status.sct, cpl->status.sc);
		ctx->failed = true;
	} else {
		fprintf(stderr, "KV command completed: cdw0=%u\n", cpl->cdw0);
	}
	ctx->done = true;
}

static int
wait_for_completion(struct kv_ctx *ctx)
{
	ctx->done = false;
	ctx->failed = false;
	while (!ctx->done) {
		spdk_nvme_qpair_process_completions(ctx->qpair, 0);
	}
	return ctx->failed ? -1 : 0;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	struct kv_ctx ctx = {};
	const struct spdk_nvme_kv_ns_data *kv_ns_data;
	uint32_t nsid;
	char *store_buf = NULL;
	char *retrieve_buf = NULL;
	const uint32_t buf_len = 256;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <vfio-user-socket-path>\n", argv[0]);
		return 1;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "kv_host";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	trid.trtype = SPDK_NVME_TRANSPORT_VFIOUSER;
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", argv[1]);

	if (spdk_nvme_probe(&trid, &ctx, probe_cb, attach_cb, NULL) != 0 || ctx.ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_probe() failed for '%s'\n", trid.traddr);
		goto out;
	}

	/* Find the first KV namespace. */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctx.ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctx.ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctx.ctrlr, nsid);

		if (ns && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
			ctx.ns = ns;
			break;
		}
	}

	if (ctx.ns == NULL) {
		fprintf(stderr, "No KV namespace found on controller\n");
		goto detach;
	}

	fprintf(stderr, "Found KV namespace nsid=%u\n", spdk_nvme_ns_get_id(ctx.ns));

	kv_ns_data = spdk_nvme_kv_ns_get_data(ctx.ns);
	if (kv_ns_data == NULL) {
		fprintf(stderr, "Failed to get KV namespace data\n");
		goto detach;
	}
	fprintf(stderr, "KV format[0]: kvkml=%u kvvml=%u mnks=%u\n",
		kv_ns_data->kvf[0].kvkml, kv_ns_data->kvf[0].kvvml, kv_ns_data->kvf[0].mnks);

	ctx.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctx.ctrlr, NULL, 0);
	if (ctx.qpair == NULL) {
		fprintf(stderr, "Failed to allocate IO qpair\n");
		goto detach;
	}

	/* I/O data buffers must live in DMA-registered memory for vfio-user. */
	store_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	retrieve_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	if (store_buf == NULL || retrieve_buf == NULL) {
		fprintf(stderr, "Failed to allocate DMA buffers\n");
		goto free_qpair;
	}
	memcpy(store_buf, g_value, sizeof(g_value));

	/* Store. */
	rc = spdk_nvme_kv_store(ctx.ns, ctx.qpair, g_key, sizeof(g_key),
				store_buf, sizeof(g_value), io_complete, &ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_kv_store submit failed: %d\n", rc);
		goto free_qpair;
	}
	if (wait_for_completion(&ctx) != 0) {
		fprintf(stderr, "KV Store failed\n");
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV Store OK (key='%s', %zu value bytes)\n", g_key, sizeof(g_value));

	/* Retrieve. */
	rc = spdk_nvme_kv_retrieve(ctx.ns, ctx.qpair, g_key, sizeof(g_key),
				   retrieve_buf, buf_len, io_complete, &ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_kv_retrieve submit failed: %d\n", rc);
		goto free_qpair;
	}
	if (wait_for_completion(&ctx) != 0) {
		fprintf(stderr, "KV Retrieve failed\n");
		rc = 1;
		goto free_qpair;
	}

	if (memcmp(retrieve_buf, g_value, sizeof(g_value)) != 0) {
		fprintf(stderr, "MISMATCH: retrieved value does not match stored value\n");
		fprintf(stderr, "  stored:    '%s'\n", g_value);
		fprintf(stderr, "  retrieved: '%s'\n", retrieve_buf);
		rc = 1;
		goto free_qpair;
	}

	fprintf(stderr, "KV Retrieve OK; bytes round-tripped: '%s'\n", retrieve_buf);
	fprintf(stderr, "PASS: KV Store/Retrieve round-trip succeeded\n");
	rc = 0;

free_qpair:
	spdk_dma_free(store_buf);
	spdk_dma_free(retrieve_buf);
	spdk_nvme_ctrlr_free_io_qpair(ctx.qpair);
detach:
	spdk_nvme_detach(ctx.ctrlr);
out:
	spdk_env_fini();
	return rc;
}
