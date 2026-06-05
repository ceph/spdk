/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * KV host (initiator) for the librados-backed kvdev end-to-end vfio-user test.
 *
 * Attaches to an NVMf controller over VFIOUSER, finds the first KV namespace,
 * prints the advertised kvvml (must be 64 MB for the rados backend), then drives
 * Store / Retrieve / Exist / Delete and asserts they persist and behave per the
 * KV spec. It also asserts that List returns the NVMe command-not-supported
 * status (Invalid Command Opcode, sc=0x01), since the rados backend defers List
 * (ADR-0002).
 *
 * Two modes (argv[2]):
 *   store  - Store g_key=g_value, then Retrieve + verify, Exist(present)=0x00,
 *            and assert List => 0x01. Leaves g_key persisted in rados.
 *   verify - Retrieve g_key + verify bytes (used after a target restart to prove
 *            the object survived in rados), then Exist(present), Delete, and
 *            Exist(absent)=0x87.
 *
 * Usage: kv_rados_host <vfio-user-socket-dir> <store|verify>
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"
#include "spdk/string.h"

/* 64 MB, the ADR-0002 rados value cap advertised as kvvml. */
#define KV_RADOS_EXPECTED_KVVML (64u * 1024 * 1024)

static const char g_key[] = "kvkey01";
static const char g_value[] = "the quick brown fox jumps over the lazy dog";

/* Oversized-value / short-buffer test (truncation reporting). A value larger
 * than the Retrieve host buffer must complete SUCCESS with cdw0 == the TRUE
 * stored length, so the host can detect truncation and resize — identical to
 * the in-memory module's BUFFER_TOO_SMALL -> SUCCESS+cdw0 mapping. */
static const char g_big_key[] = "kvbig01";
#define KV_BIG_VALUE_LEN 4096u
#define KV_SMALL_RETRIEVE_LEN 64u

struct kv_ctx {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	volatile bool		done;
	volatile bool		failed;
	volatile uint8_t	last_sct;
	volatile uint8_t	last_sc;
	volatile uint32_t	last_cdw0;
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
	ctx->last_cdw0 = cpl->cdw0;
	ctx->failed = spdk_nvme_cpl_is_error(cpl);
	ctx->done = true;
}

static void
wait_done(struct kv_ctx *ctx)
{
	while (!ctx->done) {
		spdk_nvme_qpair_process_completions(ctx->qpair, 0);
	}
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
	const char *mode;
	int rc = 1;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s <vfio-user-socket-path> <store|verify>\n", argv[0]);
		return 1;
	}
	mode = argv[2];

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "kv_rados_host";
	/* Running unprivileged (no access to physical addresses), so force the
	 * DPDK IOVA mode to VA. vfio-user loopback needs no real PCI device. */
	opts.iova_mode = "va";
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

	/* ADR-0002: the rados backend advertises a 64 MB max value length. */
	if (kv_ns_data->kvf[0].kvvml != KV_RADOS_EXPECTED_KVVML) {
		fprintf(stderr, "FAIL: kvvml=%u, expected %u (64 MB)\n",
			kv_ns_data->kvf[0].kvvml, KV_RADOS_EXPECTED_KVVML);
		goto detach;
	}
	fprintf(stderr, "kvvml OK: 64 MB max value length advertised\n");

	ctx.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctx.ctrlr, NULL, 0);
	if (ctx.qpair == NULL) {
		fprintf(stderr, "Failed to allocate IO qpair\n");
		goto detach;
	}

	store_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	retrieve_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	list_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	if (store_buf == NULL || retrieve_buf == NULL || list_buf == NULL) {
		fprintf(stderr, "Failed to allocate DMA buffers\n");
		goto free_qpair;
	}

	if (strcmp(mode, "store") == 0) {
		/* Store. */
		memcpy(store_buf, g_value, sizeof(g_value));
		ctx.done = false;
		rc = spdk_nvme_kv_store(ctx.ns, ctx.qpair, g_key, strlen(g_key),
					store_buf, sizeof(g_value), io_complete, &ctx, 0);
		if (rc != 0) {
			fprintf(stderr, "KV Store submit failed: %d\n", rc);
			goto free_qpair;
		}
		wait_done(&ctx);
		if (ctx.failed) {
			fprintf(stderr, "FAIL: KV Store sc=0x%02x\n", ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Store OK (key='%s', %zu bytes)\n", g_key, sizeof(g_value));

		/* Retrieve + verify. */
		ctx.done = false;
		rc = spdk_nvme_kv_retrieve(ctx.ns, ctx.qpair, g_key, strlen(g_key),
					   retrieve_buf, buf_len, io_complete, &ctx, 0);
		if (rc != 0) { fprintf(stderr, "Retrieve submit failed\n"); goto free_qpair; }
		wait_done(&ctx);
		if (ctx.failed || memcmp(retrieve_buf, g_value, sizeof(g_value)) != 0) {
			fprintf(stderr, "FAIL: Retrieve mismatch (sc=0x%02x, cdw0=%u)\n",
				ctx.last_sc, ctx.last_cdw0);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Retrieve OK; bytes round-tripped (cdw0=%u): '%s'\n",
			ctx.last_cdw0, retrieve_buf);

		/* Exist (present) => 0x00. */
		ctx.done = false;
		spdk_nvme_kv_exist(ctx.ns, ctx.qpair, g_key, strlen(g_key), io_complete, &ctx);
		wait_done(&ctx);
		if (ctx.last_sct != SPDK_NVME_SCT_GENERIC || ctx.last_sc != 0x00) {
			fprintf(stderr, "FAIL: Exist(present) sct=0x%02x sc=0x%02x, expected 0x00\n",
				ctx.last_sct, ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Exist(present) OK: sc=0x00\n");

		/* List must report command-not-supported (Invalid Opcode, sc=0x01). */
		ctx.done = false;
		spdk_nvme_kv_list(ctx.ns, ctx.qpair, NULL, 0, list_buf, buf_len, io_complete, &ctx);
		wait_done(&ctx);
		if (ctx.last_sct != SPDK_NVME_SCT_GENERIC || ctx.last_sc != 0x01) {
			fprintf(stderr, "FAIL: List sct=0x%02x sc=0x%02x, expected 0x01 (cmd not supported)\n",
				ctx.last_sct, ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV List OK: sc=0x01 (command not supported, deferred per ADR-0002)\n");

		/*
		 * Oversized-value / short-buffer truncation reporting.
		 * Store a KV_BIG_VALUE_LEN value, then Retrieve into a small
		 * KV_SMALL_RETRIEVE_LEN buffer. The completion must be SUCCESS
		 * (sc=0x00) with cdw0 == KV_BIG_VALUE_LEN (the TRUE length), and
		 * the first KV_SMALL_RETRIEVE_LEN bytes must match — so the host
		 * can detect truncation and re-issue with a bigger buffer.
		 */
		{
			char *big_buf = spdk_dma_zmalloc(KV_BIG_VALUE_LEN, 0, NULL);
			char *small_buf = spdk_dma_zmalloc(KV_SMALL_RETRIEVE_LEN, 0, NULL);
			uint32_t i;

			if (big_buf == NULL || small_buf == NULL) {
				fprintf(stderr, "Failed to allocate oversized-test buffers\n");
				spdk_dma_free(big_buf);
				spdk_dma_free(small_buf);
				rc = 1; goto free_qpair;
			}
			for (i = 0; i < KV_BIG_VALUE_LEN; i++) {
				big_buf[i] = (char)(i & 0xff);
			}

			ctx.done = false;
			rc = spdk_nvme_kv_store(ctx.ns, ctx.qpair, g_big_key, strlen(g_big_key),
						big_buf, KV_BIG_VALUE_LEN, io_complete, &ctx, 0);
			if (rc != 0) {
				fprintf(stderr, "Big Store submit failed\n");
				spdk_dma_free(big_buf); spdk_dma_free(small_buf);
				goto free_qpair;
			}
			wait_done(&ctx);
			if (ctx.failed) {
				fprintf(stderr, "FAIL: Big Store sc=0x%02x\n", ctx.last_sc);
				spdk_dma_free(big_buf); spdk_dma_free(small_buf);
				rc = 1; goto free_qpair;
			}
			fprintf(stderr, "KV Store(big) OK (key='%s', %u bytes)\n",
				g_big_key, KV_BIG_VALUE_LEN);

			ctx.done = false;
			rc = spdk_nvme_kv_retrieve(ctx.ns, ctx.qpair, g_big_key, strlen(g_big_key),
						   small_buf, KV_SMALL_RETRIEVE_LEN, io_complete, &ctx, 0);
			if (rc != 0) {
				fprintf(stderr, "Big Retrieve submit failed\n");
				spdk_dma_free(big_buf); spdk_dma_free(small_buf);
				goto free_qpair;
			}
			wait_done(&ctx);
			fprintf(stderr, "### Oversized Retrieve: buf_len=%u true_len=%u observed cdw0=%u sct=0x%02x sc=0x%02x\n",
				KV_SMALL_RETRIEVE_LEN, KV_BIG_VALUE_LEN, ctx.last_cdw0,
				ctx.last_sct, ctx.last_sc);
			if (ctx.failed || ctx.last_sct != SPDK_NVME_SCT_GENERIC ||
			    ctx.last_sc != 0x00) {
				fprintf(stderr, "FAIL: oversized Retrieve not SUCCESS (sct=0x%02x sc=0x%02x)\n",
					ctx.last_sct, ctx.last_sc);
				spdk_dma_free(big_buf); spdk_dma_free(small_buf);
				rc = 1; goto free_qpair;
			}
			if (ctx.last_cdw0 != KV_BIG_VALUE_LEN) {
				fprintf(stderr, "FAIL: cdw0=%u, expected TRUE length %u (truncation not reported)\n",
					ctx.last_cdw0, KV_BIG_VALUE_LEN);
				spdk_dma_free(big_buf); spdk_dma_free(small_buf);
				rc = 1; goto free_qpair;
			}
			if (memcmp(small_buf, big_buf, KV_SMALL_RETRIEVE_LEN) != 0) {
				fprintf(stderr, "FAIL: truncated bytes mismatch in oversized Retrieve\n");
				spdk_dma_free(big_buf); spdk_dma_free(small_buf);
				rc = 1; goto free_qpair;
			}
			fprintf(stderr, "KV Retrieve(oversized) OK: SUCCESS, cdw0=%u == true length %u, "
				"first %u bytes correct (truncation reported, matches in-mem module)\n",
				ctx.last_cdw0, KV_BIG_VALUE_LEN, KV_SMALL_RETRIEVE_LEN);

			/* Clean up the big object so the test is repeatable. */
			ctx.done = false;
			spdk_nvme_kv_delete(ctx.ns, ctx.qpair, g_big_key, strlen(g_big_key),
					    io_complete, &ctx);
			wait_done(&ctx);
			spdk_dma_free(big_buf);
			spdk_dma_free(small_buf);
		}

		fprintf(stderr, "PASS(store): Store/Retrieve/Exist + List-not-supported + oversized-truncation OK; key persisted\n");
		rc = 0;
	} else if (strcmp(mode, "verify") == 0) {
		/* Retrieve g_key (must have survived target restart in rados) + verify. */
		ctx.done = false;
		rc = spdk_nvme_kv_retrieve(ctx.ns, ctx.qpair, g_key, strlen(g_key),
					   retrieve_buf, buf_len, io_complete, &ctx, 0);
		if (rc != 0) { fprintf(stderr, "Retrieve submit failed\n"); goto free_qpair; }
		wait_done(&ctx);
		if (ctx.failed || memcmp(retrieve_buf, g_value, sizeof(g_value)) != 0) {
			fprintf(stderr, "FAIL: post-restart Retrieve mismatch (sc=0x%02x)\n", ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Retrieve(after restart) OK; value survived: '%s'\n", retrieve_buf);

		/* Exist(present) => 0x00. */
		ctx.done = false;
		spdk_nvme_kv_exist(ctx.ns, ctx.qpair, g_key, strlen(g_key), io_complete, &ctx);
		wait_done(&ctx);
		if (ctx.last_sc != 0x00) {
			fprintf(stderr, "FAIL: Exist(present) sc=0x%02x, expected 0x00\n", ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Exist(present) OK: sc=0x00\n");

		/* Delete => 0x00. */
		ctx.done = false;
		spdk_nvme_kv_delete(ctx.ns, ctx.qpair, g_key, strlen(g_key), io_complete, &ctx);
		wait_done(&ctx);
		if (ctx.last_sc != 0x00) {
			fprintf(stderr, "FAIL: Delete sc=0x%02x, expected 0x00\n", ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Delete OK: sc=0x00\n");

		/* Exist(absent) => 0x87 (KV Key Does Not Exist). */
		ctx.done = false;
		spdk_nvme_kv_exist(ctx.ns, ctx.qpair, g_key, strlen(g_key), io_complete, &ctx);
		wait_done(&ctx);
		if (ctx.last_sc != 0x87) {
			fprintf(stderr, "FAIL: Exist(absent) sc=0x%02x, expected 0x87\n", ctx.last_sc);
			rc = 1; goto free_qpair;
		}
		fprintf(stderr, "KV Exist(absent) OK: sc=0x87 (KEY_DOES_NOT_EXIST)\n");

		fprintf(stderr, "PASS(verify): persistence-after-restart + Delete/Exist OK\n");
		rc = 0;
	} else {
		fprintf(stderr, "Unknown mode '%s' (use store|verify)\n", mode);
		rc = 1;
	}

free_qpair:
	spdk_dma_free(store_buf);
	spdk_dma_free(retrieve_buf);
	spdk_dma_free(list_buf);
	if (ctx.qpair) {
		spdk_nvme_ctrlr_free_io_qpair(ctx.qpair);
	}
detach:
	spdk_nvme_detach(ctx.ctrlr);
out:
	spdk_env_fini();
	return rc;
}
