/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "common/lib/test_env.c"
#include "nvmf/vfio_user.c"
#include "nvmf/transport.c"

DEFINE_STUB(spdk_nvmf_ctrlr_get_regs, const struct spdk_nvmf_registers *,
	    (struct spdk_nvmf_ctrlr *ctrlr), NULL);
DEFINE_STUB(spdk_mem_register, int, (void *vaddr, size_t len), 0);
DEFINE_STUB(spdk_mem_unregister, int, (void *vaddr, size_t len), 0);
DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB_V(spdk_nvmf_tgt_new_qpair, (struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair));
DEFINE_STUB(nvmf_ctrlr_abort_request, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(spdk_nvmf_qpair_disconnect, int, (struct spdk_nvmf_qpair *qpair), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_nqn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns), NULL);
DEFINE_STUB(spdk_bdev_desc_get_block_size, uint32_t, (struct spdk_bdev_desc *desc), 512);
DEFINE_STUB(spdk_bdev_get_numa_id, int32_t, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_nvmf_subsystem_pause, int, (struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid, spdk_nvmf_subsystem_state_change_done cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_resume, int, (struct spdk_nvmf_subsystem *subsystem,
		spdk_nvmf_subsystem_state_change_done cb_fn, void *cb_arg), 0);
DEFINE_STUB_V(spdk_nvmf_ctrlr_abort_aer, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB(spdk_nvmf_ctrlr_async_event_error_event, int, (struct spdk_nvmf_ctrlr *ctrlr,
		enum spdk_nvme_async_event_info_error info), 0);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);
DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid, int, (struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB(nvmf_subsystem_get_ctrlr, struct spdk_nvmf_ctrlr *,
	    (struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid), NULL);
DEFINE_STUB_V(nvmf_ctrlr_set_fatal_status, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB(spdk_nvmf_ctrlr_save_migr_data, int, (struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvmf_ctrlr_migr_data *data), 0);
DEFINE_STUB(spdk_nvmf_ctrlr_restore_migr_data, int, (struct spdk_nvmf_ctrlr *ctrlr,
		const struct spdk_nvmf_ctrlr_migr_data *data), 0);
DEFINE_STUB(spdk_mempool_lookup, struct spdk_mempool *, (const char *name), NULL);
DEFINE_STUB(nvmf_subsystem_gen_cntlid, uint16_t, (struct spdk_nvmf_subsystem *subsystem), 1)

static void *
gpa_to_vva(void *prv, uint64_t addr, uint64_t len, uint32_t flags)
{
	return (void *)(uintptr_t)addr;
}

/*
 * Models a host where each guest page sits in its own vfio-user DMA region: a
 * mapping can only translate one page at a time, mimicking vfu_addr_to_sgl()
 * reporting "multiple segments needed" for any larger range. Forces
 * nvme_prp_emit_run() to split a coalesced run back into per-page iovs.
 */
static void *
gpa_to_vva_paged(void *prv, uint64_t addr, uint64_t len, uint32_t flags)
{
	if (len > 4096) {
		return NULL;
	}
	return (void *)(uintptr_t)addr;
}

static void
test_nvme_cmd_map_prps(void)
{
	struct spdk_nvme_cmd cmd = {};
	struct iovec iovs[33];
	uint64_t phy_addr, *prp, *prp_page1;
	uint32_t len;
	void *prps;
	int i, ret;
	size_t mps = 4096;
	/*
	 * gpa_to_vva() is the identity here, so the *data* pages need not be backed
	 * by real memory; we use a fake page-aligned contiguous device base. Only
	 * the PRP-list pages (prps) are actually dereferenced, so those are real.
	 */
	uint64_t base = 0x100000000ULL;

	/* Two pages so we can build a *chained* PRP list (case 5). */
	prps = spdk_zmalloc(2 * 4096, 4096, &phy_addr, 0, 0);
	CU_ASSERT(prps != NULL);

	/* case 1: 4KiB, PRP1 only */
	cmd.dptr.prp.prp1 = base;
	len = 4096;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)base);
	CU_ASSERT(iovs[0].iov_len == 4096);

	/* case 2: 4KiB split across PRP1 (1KiB) + a direct PRP2 (3KiB); also too
	 * few iovs returns -ERANGE. (PRP2-direct path; no coalescing.) */
	cmd.dptr.prp.prp1 = base + 1024 * 3;
	cmd.dptr.prp.prp2 = base + 4096;
	len = 4096;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 1, len, mps, gpa_to_vva);
	CU_ASSERT(ret == -ERANGE);
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 2);
	CU_ASSERT(iovs[0].iov_len == 1024);
	CU_ASSERT(iovs[1].iov_len == 1024 * 3);

	/* case 3: 128KiB via a single PRP-list page, fully contiguous. Coalesces to
	 * the PRP1 partial-page iov + one big data iov. */
	cmd.dptr.prp.prp1 = base + 1024 * 3;
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)prps;
	len = 128 * 1024;
	prp = prps;
	for (i = 1; i < 33; i++) {
		prp[i - 1] = base + (uint64_t)i * 4096;
	}
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)(base + 1024 * 3));
	CU_ASSERT(iovs[0].iov_len == 1024);
	CU_ASSERT(iovs[1].iov_base == (void *)(uintptr_t)(base + 4096));
	CU_ASSERT(iovs[1].iov_len == 128 * 1024 - 1024);

	/* case 4: non-contiguous PRP list => one iov per discontiguity. 32 distinct,
	 * non-adjacent data pages + PRP1 == 33 iovs (exactly max_iovcnt). */
	cmd.dptr.prp.prp1 = base; /* aligned: full first page */
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)prps;
	len = 33 * 4096; /* PRP1 + 32 list-described pages */
	prp = prps;
	for (i = 0; i < 32; i++) {
		prp[i] = base + (uint64_t)(100 - i) * 0x10000ULL; /* descending, gapped */
	}
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 33);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)base);
	CU_ASSERT(iovs[1].iov_base == (void *)(uintptr_t)(base + 100 * 0x10000ULL));
	CU_ASSERT(iovs[32].iov_len == 4096);
	/* one more discontiguous page would need a 34th iov => -ERANGE */
	len = 34 * 4096;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == -ERANGE);

	/* case 5: CHAINED PRP list, fully contiguous, 4 MiB (1024 pages). List page
	 * 0 holds 511 data entries + a chain pointer to list page 1 (512 entries).
	 * Contiguous => coalesces to PRP1 + a single 4 MiB-4 KiB data iov. Exercises
	 * the chain-following path. */
	cmd.dptr.prp.prp1 = base;
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)prps;
	len = 4 * 1024 * 1024; /* 1024 pages */
	prp = prps;                                        /* list page 0 */
	prp_page1 = (uint64_t *)((uintptr_t)prps + 4096);  /* list page 1 */
	for (i = 0; i < 511; i++) {
		prp[i] = base + (uint64_t)(i + 1) * 4096;  /* data pages 1..511 */
	}
	prp[511] = (uint64_t)(uintptr_t)prp_page1;         /* chain pointer */
	for (i = 0; i < 512; i++) {
		prp_page1[i] = base + (uint64_t)(512 + i) * 4096; /* data pages 512..1023 */
	}
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)base);
	CU_ASSERT(iovs[0].iov_len == 4096);
	CU_ASSERT(iovs[1].iov_base == (void *)(uintptr_t)(base + 4096));
	CU_ASSERT(iovs[1].iov_len == (size_t)(4 * 1024 * 1024 - 4096));

	/* case 6: physically-contiguous data, but the host can map only one page per
	 * DMA region (gpa_to_vva_paged). The walk coalesces all 32 listed pages into
	 * one run; nvme_prp_emit_run then splits that run back into 32 per-page iovs
	 * (+ PRP1 == 33, exactly max_iovcnt). Exercises the region-boundary split. */
	cmd.dptr.prp.prp1 = base; /* aligned: full first page */
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)prps;
	prp = prps;
	for (i = 0; i < 33; i++) {
		prp[i] = base + (uint64_t)(i + 1) * 4096; /* contiguous data pages */
	}
	len = 33 * 4096; /* PRP1 + 32 listed pages */
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva_paged);
	CU_ASSERT(ret == 33);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)base);
	CU_ASSERT(iovs[0].iov_len == 4096);
	CU_ASSERT(iovs[1].iov_base == (void *)(uintptr_t)(base + 4096));
	CU_ASSERT(iovs[1].iov_len == 4096);
	CU_ASSERT(iovs[32].iov_base == (void *)(uintptr_t)(base + 32 * 4096));
	CU_ASSERT(iovs[32].iov_len == 4096);
	/* one more contiguous page splits into a 34th iov => -ERANGE */
	len = 34 * 4096;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva_paged);
	CU_ASSERT(ret == -ERANGE);

	spdk_free(prps);
}

static void
test_nvme_cmd_map_sgls(void)
{
	struct spdk_nvme_cmd cmd = {};
	struct iovec iovs[33];
	uint64_t phy_addr;
	uint32_t len;
	void *buf, *sgls;
	struct spdk_nvme_sgl_descriptor *sgl;
	int i, ret;
	size_t mps = 4096;

	buf = spdk_zmalloc(132 * 1024, 4096, &phy_addr, 0, 0);
	CU_ASSERT(buf != NULL);
	sgls = spdk_zmalloc(4096, 4096, &phy_addr, 0, 0);
	CU_ASSERT(sgls != NULL);

	/* test case 1: 8KiB with 1 data block */
	len = 8192;
	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	cmd.dptr.sgl1.unkeyed.length = len;
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)buf;

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 1);
	CU_ASSERT(iovs[0].iov_base == buf);
	CU_ASSERT(iovs[0].iov_len == 8192);

	/* test case 2: 8KiB with 2 data blocks and 1 last segment */
	sgl = (struct spdk_nvme_sgl_descriptor *)sgls;
	sgl[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[0].unkeyed.length = 2048;
	sgl[0].address = (uint64_t)(uintptr_t)buf;
	sgl[1].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[1].unkeyed.length = len - 2048;
	sgl[1].address = (uint64_t)(uintptr_t)buf + 16 * 1024;

	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	cmd.dptr.sgl1.unkeyed.length = 2 * sizeof(*sgl);
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)sgls;

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)buf);
	CU_ASSERT(iovs[0].iov_len == 2048);
	CU_ASSERT(iovs[1].iov_base == (void *)((uintptr_t)buf + 16 * 1024));
	CU_ASSERT(iovs[1].iov_len == len - 2048);

	/* test case 3: 8KiB with 1 segment, 1 last segment and 3 data blocks */
	sgl[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[0].unkeyed.length = 2048;
	sgl[0].address = (uint64_t)(uintptr_t)buf;
	sgl[1].unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	sgl[1].unkeyed.length = 2 * sizeof(*sgl);
	sgl[1].address = (uint64_t)(uintptr_t)&sgl[9];

	sgl[9].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[9].unkeyed.length = 4096;
	sgl[9].address = (uint64_t)(uintptr_t)buf + 4 * 1024;
	sgl[10].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[10].unkeyed.length = 2048;
	sgl[10].address = (uint64_t)(uintptr_t)buf + 16 * 1024;

	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_SEGMENT;
	cmd.dptr.sgl1.unkeyed.length = 2 * sizeof(*sgl);
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)&sgl[0];

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)buf);
	CU_ASSERT(iovs[0].iov_len == 2048);
	CU_ASSERT(iovs[1].iov_base == (void *)((uintptr_t)buf + 4 * 1024));
	CU_ASSERT(iovs[1].iov_len == 4096);
	CU_ASSERT(iovs[2].iov_base == (void *)((uintptr_t)buf + 16 * 1024));
	CU_ASSERT(iovs[2].iov_len == 2048);

	/* test case 4: not enough iovs */
	len = 12 * 1024;
	for (i = 0; i < 6; i++) {
		sgl[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl[0].unkeyed.length = 2048;
		sgl[0].address = (uint64_t)(uintptr_t)buf + i * 4096;
	}

	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	cmd.dptr.sgl1.unkeyed.length = 6 * sizeof(*sgl);
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)sgls;

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 4, len, mps, gpa_to_vva);
	CU_ASSERT(ret == -ERANGE);

	spdk_free(buf);
	spdk_free(sgls);
}

static void
ut_transport_destroy_done_cb(void *cb_arg)
{
	int *done = cb_arg;
	*done = 1;
}

static void
test_nvmf_vfio_user_create_destroy(void)
{
	struct spdk_nvmf_transport *transport = NULL;
	struct nvmf_vfio_user_transport *vu_transport = NULL;
	struct nvmf_vfio_user_endpoint *endpoint = NULL;
	struct spdk_nvmf_transport_opts opts = {};
	int done;

	/* Initialize transport_specific NULL to avoid decoding json */
	opts.transport_specific = NULL;

	/* A max_io_size beyond what the per-request scatter-gather list can map
	 * (NVMF_VFIO_USER_MAX_IOVECS) is rejected at create time; 128 MiB is
	 * within budget and is exercised by the create/destroy path below. */
	opts.max_io_size = 512 * 1024 * 1024;
	CU_ASSERT(nvmf_vfio_user_create(&opts) == NULL);
	opts.max_io_size = 128 * 1024 * 1024;

	transport = nvmf_vfio_user_create(&opts);
	CU_ASSERT(transport != NULL);

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);
	/* Allocate a endpoint for destroy */
	endpoint = calloc(1, sizeof(*endpoint));
	pthread_mutex_init(&endpoint->lock, NULL);
	TAILQ_INSERT_TAIL(&vu_transport->endpoints, endpoint, link);
	done = 0;

	nvmf_vfio_user_destroy(transport, ut_transport_destroy_done_cb, &done);
	CU_ASSERT(done == 1);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("vfio_user", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_cmd_map_prps);
	CU_ADD_TEST(suite, test_nvme_cmd_map_sgls);
	CU_ADD_TEST(suite, test_nvmf_vfio_user_create_destroy);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
