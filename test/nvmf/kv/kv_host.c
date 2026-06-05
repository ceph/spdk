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

/* Extra keys stored to exercise the KV List command. g_key above is also
 * expected to come back from List. */
static const char *const g_list_keys[] = {
	"alpha", "bravo", "charlie", "delta",
};
#define NUM_LIST_KEYS (sizeof(g_list_keys) / sizeof(g_list_keys[0]))

/* TTL (seconds) stored via the vendor _ext API (ADR-0003). The shell driver
 * reads it back from the target through the kvdev_mem_get_entry RPC and asserts
 * it round-tripped. */
#define KV_TEST_TTL_SECONDS 4242

struct kv_ctx {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	volatile bool		done;
	volatile bool		failed;
	/* Status code (sct/sc) of the most recently completed command. */
	volatile uint8_t	last_sct;
	volatile uint8_t	last_sc;
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

	ctx->last_sct = cpl->status.sct;
	ctx->last_sc = cpl->status.sc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "KV command failed: sct=%d sc=%d\n",
			cpl->status.sct, cpl->status.sc);
		ctx->failed = true;
	} else {
		fprintf(stderr, "KV command completed: cdw0=%u\n", cpl->cdw0);
	}
	ctx->done = true;
}

/*
 * Submit a no-data KV command (Delete or Exist) and wait for it, returning the
 * NVMe status code (sc) the device reported. sct is asserted to be GENERIC.
 */
static int
run_no_data(struct kv_ctx *ctx, int (*submit)(struct spdk_nvme_ns *,
		struct spdk_nvme_qpair *, const void *, uint8_t,
		spdk_nvme_cmd_cb, void *), const char *what)
{
	int rc;

	ctx->done = false;
	ctx->failed = false;
	rc = submit(ctx->ns, ctx->qpair, g_key, strlen(g_key), io_complete, ctx);
	if (rc != 0) {
		fprintf(stderr, "%s submit failed: %d\n", what, rc);
		return -1;
	}
	while (!ctx->done) {
		spdk_nvme_qpair_process_completions(ctx->qpair, 0);
	}
	if (ctx->last_sct != SPDK_NVME_SCT_GENERIC) {
		fprintf(stderr, "%s: unexpected sct=%u\n", what, ctx->last_sct);
		return -1;
	}
	return ctx->last_sc;
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
	uint8_t *list_buf = NULL;
	const uint32_t buf_len = 256;
	uint32_t i;
	int rc = 1;
	int sc;

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

	/* Vendor TTL capability (ADR-0003) is advertised in the KV Identify NS
	 * vendor-specific capability byte. The shell driver greps for this line. */
	if (kv_ns_data->vs_cap & SPDK_NVME_KV_NS_VS_CAP_TTL) {
		fprintf(stderr, "KV TTL capability: SUPPORTED (vs_cap=0x%02x)\n", kv_ns_data->vs_cap);
	} else {
		fprintf(stderr, "KV TTL capability: NOT SUPPORTED (vs_cap=0x%02x)\n", kv_ns_data->vs_cap);
		goto detach;
	}

	ctx.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctx.ctrlr, NULL, 0);
	if (ctx.qpair == NULL) {
		fprintf(stderr, "Failed to allocate IO qpair\n");
		goto detach;
	}

	/* I/O data buffers must live in DMA-registered memory for vfio-user. */
	store_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	retrieve_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	list_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	if (store_buf == NULL || retrieve_buf == NULL || list_buf == NULL) {
		fprintf(stderr, "Failed to allocate DMA buffers\n");
		goto free_qpair;
	}
	memcpy(store_buf, g_value, sizeof(g_value));

	/* Store with a vendor TTL via the _ext API (ADR-0003). */
	struct spdk_nvme_kv_store_ext_opts store_opts;
	spdk_nvme_kv_store_ext_opts_init(&store_opts, sizeof(store_opts));
	store_opts.ttl = KV_TEST_TTL_SECONDS;
	rc = spdk_nvme_kv_store_ext(ctx.ns, ctx.qpair, g_key, strlen(g_key),
				    store_buf, sizeof(g_value), io_complete, &ctx, 0, &store_opts);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_kv_store_ext submit failed: %d\n", rc);
		goto free_qpair;
	}
	if (wait_for_completion(&ctx) != 0) {
		fprintf(stderr, "KV Store failed\n");
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV Store OK (key='%s', %zu value bytes, ttl=%u)\n",
		g_key, sizeof(g_value), (unsigned)KV_TEST_TTL_SECONDS);

	/* Retrieve. */
	rc = spdk_nvme_kv_retrieve(ctx.ns, ctx.qpair, g_key, strlen(g_key),
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

	/*
	 * Slice 2: Exist + Delete. With the key present, Exist must report
	 * SUCCESS (00h). After Delete, Exist must report KV Key Does Not Exist
	 * (87h). Each assertion checks the exact NVMe status code from the CQE.
	 */
	sc = run_no_data(&ctx, spdk_nvme_kv_exist, "KV Exist (present)");
	if (sc != SPDK_NVME_SC_SUCCESS) {
		fprintf(stderr, "FAIL: Exist on present key returned sc=0x%02x, expected 0x00\n", sc);
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV Exist (present) OK: sc=0x00 (SUCCESS)\n");

	sc = run_no_data(&ctx, spdk_nvme_kv_delete, "KV Delete");
	if (sc != SPDK_NVME_SC_SUCCESS) {
		fprintf(stderr, "FAIL: Delete of present key returned sc=0x%02x, expected 0x00\n", sc);
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV Delete OK: sc=0x00 (SUCCESS)\n");

	sc = run_no_data(&ctx, spdk_nvme_kv_exist, "KV Exist (absent)");
	if (sc != SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST) {
		fprintf(stderr, "FAIL: Exist on absent key returned sc=0x%02x, expected 0x87\n", sc);
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV Exist (absent) OK: sc=0x87 (KEY_DOES_NOT_EXIST)\n");

	/* A Delete of the now-absent key must also report 87h. */
	sc = run_no_data(&ctx, spdk_nvme_kv_delete, "KV Delete (absent)");
	if (sc != SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST) {
		fprintf(stderr, "FAIL: Delete of absent key returned sc=0x%02x, expected 0x87\n", sc);
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV Delete (absent) OK: sc=0x87 (KEY_DOES_NOT_EXIST)\n");

	fprintf(stderr, "PASS: KV Store/Retrieve/Exist/Delete sequence succeeded\n");

	/*
	 * Slice 2 deleted g_key above; the List check below expects g_key to be
	 * present (expected = NUM_LIST_KEYS + 1), so re-store it before listing.
	 * Re-store via the vendor _ext API with the TTL so g_key's FINAL state
	 * carries ttl=KV_TEST_TTL_SECONDS: the shell driver reads it back through
	 * the kvdev_mem_get_entry RPC after this host process exits (ADR-0003).
	 */
	memcpy(store_buf, g_value, sizeof(g_value));
	spdk_nvme_kv_store_ext_opts_init(&store_opts, sizeof(store_opts));
	store_opts.ttl = KV_TEST_TTL_SECONDS;
	rc = spdk_nvme_kv_store_ext(ctx.ns, ctx.qpair, g_key, strlen(g_key),
				    store_buf, sizeof(g_value), io_complete, &ctx, 0, &store_opts);
	if (rc != 0 || wait_for_completion(&ctx) != 0) {
		fprintf(stderr, "KV re-store of g_key before List failed\n");
		rc = 1;
		goto free_qpair;
	}
	fprintf(stderr, "KV re-store of g_key (with TTL) before List OK\n");

	/* Store several more keys, then List them all back. */
	for (i = 0; i < NUM_LIST_KEYS; i++) {
		uint8_t klen = (uint8_t)strlen(g_list_keys[i]);

		memset(store_buf, 0, buf_len);
		store_buf[0] = 'x';
		rc = spdk_nvme_kv_store(ctx.ns, ctx.qpair, g_list_keys[i], klen,
					store_buf, 1, io_complete, &ctx, 0);
		if (rc != 0 || wait_for_completion(&ctx) != 0) {
			fprintf(stderr, "KV Store of list key '%s' failed\n", g_list_keys[i]);
			rc = 1;
			goto free_qpair;
		}
	}
	fprintf(stderr, "Stored %u additional keys for List\n", (unsigned)NUM_LIST_KEYS);

	/* List from the beginning (NULL start key). */
	memset(list_buf, 0, buf_len);
	rc = spdk_nvme_kv_list(ctx.ns, ctx.qpair, NULL, 0, list_buf, buf_len,
			       io_complete, &ctx);
	if (rc != 0 || wait_for_completion(&ctx) != 0) {
		fprintf(stderr, "KV List failed\n");
		rc = 1;
		goto free_qpair;
	}

	/* Parse the return data structure: 4-byte NRK, then per key
	 * { 2-byte KL, key bytes, pad to 4-byte boundary }. Verify every key we
	 * expect (g_key + the list keys) is present. */
	{
		uint32_t nrk, off = 4, found = 0, expected = NUM_LIST_KEYS + 1;
		const char *all_keys[NUM_LIST_KEYS + 1];
		uint8_t all_lens[NUM_LIST_KEYS + 1];
		bool seen[NUM_LIST_KEYS + 1] = { false };
		uint32_t j;

		/* All keys (g_key and the list keys) are stored with strlen, i.e.
		 * without the NUL terminator, so List reports them at strlen length.
		 * This also matches the kvdev_mem_get_entry RPC, which looks up
		 * g_key by strlen for the TTL round-trip assertion. */
		all_keys[0] = g_key;
		all_lens[0] = (uint8_t)strlen(g_key);
		for (j = 0; j < NUM_LIST_KEYS; j++) {
			all_keys[j + 1] = g_list_keys[j];
			all_lens[j + 1] = (uint8_t)strlen(g_list_keys[j]);
		}

		memcpy(&nrk, list_buf, sizeof(nrk));
		fprintf(stderr, "KV List returned NRK=%u\n", nrk);

		for (i = 0; i < nrk && off + 2 <= buf_len; i++) {
			uint16_t kl;

			memcpy(&kl, list_buf + off, sizeof(kl));
			off += sizeof(kl);
			if (off + kl > buf_len) {
				break;
			}
			for (j = 0; j < expected; j++) {
				if (!seen[j] && all_lens[j] == kl &&
				    memcmp(list_buf + off, all_keys[j], kl) == 0) {
					seen[j] = true;
					found++;
					break;
				}
			}
			/* Advance past the key and pad to the 4-byte boundary. */
			off += kl;
			off = (off + 3) & ~3u;
		}

		if (nrk != expected || found != expected) {
			fprintf(stderr, "KV List MISMATCH: nrk=%u found=%u expected=%u\n",
				nrk, found, expected);
			rc = 1;
			goto free_qpair;
		}
		fprintf(stderr, "KV List OK; all %u keys returned\n", expected);
	}

	fprintf(stderr, "PASS: KV Store/Retrieve/Exist/Delete/List round-trip succeeded\n");
	rc = 0;

free_qpair:
	spdk_dma_free(store_buf);
	spdk_dma_free(retrieve_buf);
	spdk_dma_free(list_buf);
	spdk_nvme_ctrlr_free_io_qpair(ctx.qpair);
detach:
	spdk_nvme_detach(ctx.ctrlr);
out:
	spdk_env_fini();
	return rc;
}
