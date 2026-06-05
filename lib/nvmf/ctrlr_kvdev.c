/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_transport.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/util.h"

#include "nvmf_internal.h"

/*
 * Key-Value command set dispatch for NVMf.
 *
 * This is the KV analogue of ctrlr_bdev.c: it decodes the NVMe KV Store and
 * Retrieve commands (the key encoding mirrors lib/nvme/nvme_kv.c's
 * nvme_kv_cmd_set_key()) and drives the bound kvdev.
 *
 * Key encoding in the SQE (per the KV Command Set Specification):
 *   - Key length: CDW11 bits 7:0
 *   - Key bytes [7:0]:   CDW2  (bits 31:0), CDW3  (bits 63:32)
 *   - Key bytes [15:8]:  CDW14 (bits 95:64), CDW15 (bits 127:96)
 *   - Value/host buffer size: CDW10
 */

struct nvmf_kvdev_request {
	struct spdk_nvmf_request	*req;
	/* Bounce buffer used when the request payload spans multiple iovs. */
	void				*bounce;
	/* Host buffer size from CDW10 (bytes the host actually offered). The
	 * bounce buffer is only valid up to this many bytes. */
	uint32_t			xfer_len;

	/* List-only: where the return data structure is being assembled (either
	 * the single request iov or the bounce buffer) and how it is filling up. */
	uint8_t				*list_buf;
	/* Bytes of list_buf already consumed by the NRK header + emitted keys. */
	uint32_t			list_off;
	/* Total bytes available in list_buf (== host buffer size, CDW10). */
	uint32_t			list_cap;
};

/* List return data structure layout (KV spec §3.2.2.2, Figures 15/16):
 *   uint32_t Number of Returned Keys (NRK)
 *   repeated { uint16_t Key Length; key bytes; pad to 4-byte boundary }
 */
#define NVMF_KV_LIST_NRK_SIZE 4

/* Bytes occupied by one key entry (2-byte KL + key + pad to 4-byte boundary). */
static inline uint32_t
nvmf_kv_list_entry_size(uint8_t key_len)
{
	return SPDK_ALIGN_CEIL(sizeof(uint16_t) + key_len, 4);
}

void
nvmf_kvdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_kv_ns_data *nsdata)
{
	const struct spdk_kvdev_caps *caps = spdk_kvdev_get_caps(ns->kvdev);

	/* One KV format (format index 0). nkvf is 0's based. */
	nsdata->nkvf = 0;
	nsdata->kvfc.kvfi = 0;

	nsdata->kvf[0].kvkml = caps->max_key_len;
	nsdata->kvf[0].kvvml = caps->max_value_len;
	nsdata->kvf[0].mnks = caps->max_num_keys;

	/* nsze/nuse are reported in bytes for KV namespaces. We do not track a
	 * fixed capacity for the in-memory backend, so advertise the max value
	 * length as a nominal namespace size. TODO: surface real capacity once
	 * a kvdev exposes it. */
	nsdata->nsze = caps->max_value_len;
	nsdata->nuse = 0;

	/* Advertise the vendor TTL extension (ADR-0003) so a host can detect
	 * that KV Store honours the TTL Valid Store Option + CDW12 TTL. The TTL
	 * is store-only (persisted, never enforced). */
	nsdata->vs_cap |= SPDK_NVME_KV_NS_VS_CAP_TTL;
}

void
nvmf_kvdev_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_kv_ctrlr_data *cdata)
{
	cdata->ver = SPDK_NVME_KV_SPEC_VER;
}

static void
nvmf_kvdev_decode_key(const struct spdk_nvme_cmd *cmd, uint8_t *key, uint8_t *key_len)
{
	uint8_t len = cmd->cdw11_bits.kv.kl;

	*key_len = len;
	if (len == 0) {
		return;
	}

	/* Low 64 bits live in CDW2/CDW3; high 64 bits in CDW14/CDW15. */
	memcpy(key, (const uint8_t *)&cmd->cdw2, spdk_min(len, 8));
	if (len > 8) {
		memcpy(key + 8, (const uint8_t *)&cmd->cdw14, spdk_min(len - 8, 8));
	}
}

static void
nvmf_kvdev_complete(struct nvmf_kvdev_request *kv_req, int kvstatus, uint32_t value_len)
{
	struct spdk_nvmf_request *req = kv_req->req;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;

	switch (kvstatus) {
	case SPDK_KVDEV_IO_STATUS_SUCCESS:
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		/* For Retrieve the value length is returned in CQE DW0. */
		rsp->cdw0 = value_len;
		break;
	case SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST:
		rsp->status.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
		break;
	case SPDK_KVDEV_IO_STATUS_KEY_EXIST:
		rsp->status.sc = SPDK_NVME_SC_KEY_EXISTS;
		break;
	case SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL:
		/* Spec: device returns requested portion and reports the full
		 * value length in the CQE; the command itself succeeds. */
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		rsp->cdw0 = value_len;
		break;
	case SPDK_KVDEV_IO_STATUS_INVALID:
		rsp->status.sc = SPDK_NVME_SC_INVALID_VALUE_SIZE;
		break;
	case SPDK_KVDEV_IO_STATUS_NOMEM:
		rsp->status.sc = SPDK_NVME_SC_CAPACITY_EXCEEDED;
		break;
	case SPDK_KVDEV_IO_STATUS_FAILED:
	default:
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		break;
	}

	free(kv_req->bounce);
	free(kv_req);
	spdk_nvmf_request_complete(req);
}

static void
nvmf_kvdev_store_done(void *cb_arg, int status, uint32_t value_len)
{
	nvmf_kvdev_complete(cb_arg, status, value_len);
}

/*
 * Completion for the no-data KV ops (Delete, Exist): there is no payload to
 * scatter back, so just translate the status. value_len is unused.
 */
static void
nvmf_kvdev_simple_done(void *cb_arg, int status, uint32_t value_len)
{
	nvmf_kvdev_complete(cb_arg, status, value_len);
}

static void
nvmf_kvdev_retrieve_done(void *cb_arg, int status, uint32_t value_len)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	struct spdk_nvmf_request *req = kv_req->req;

	/* If the payload spanned multiple iovs we retrieved into a bounce
	 * buffer; scatter it back out to the request iovs. Only copy bytes the
	 * kvdev actually wrote (bounded by the host buffer size from CDW10), not
	 * req->length, so a host that advertises an SGL larger than CDW10 cannot
	 * read back uninitialized bounce-buffer bytes. */
	if (kv_req->bounce != NULL &&
	    (status == SPDK_KVDEV_IO_STATUS_SUCCESS ||
	     status == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL)) {
		spdk_copy_buf_to_iovs(req->iov, req->iovcnt, kv_req->bounce,
				      spdk_min(value_len, kv_req->xfer_len));
	}

	nvmf_kvdev_complete(kv_req, status, value_len);
}

/*
 * Per-key callback for List. Appends one { KL, key, pad } entry to the return
 * data structure being assembled in kv_req->list_buf, immediately after the
 * 4-byte NRK header. Returns false (stop) as soon as the next whole key would
 * not fit in the host buffer, so the structure only ever contains complete
 * keys (KV spec: number returned = keys that completely fit).
 */
static bool
nvmf_kvdev_list_iter(void *cb_arg, const void *key, uint8_t key_len)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	uint32_t entry_size = nvmf_kv_list_entry_size(key_len);
	uint8_t *p;
	uint16_t kl16 = key_len;

	if (kv_req->list_off + entry_size > kv_req->list_cap) {
		/* This whole key would not fit; stop without emitting it. */
		return false;
	}

	p = kv_req->list_buf + kv_req->list_off;
	memcpy(p, &kl16, sizeof(kl16));
	memcpy(p + sizeof(kl16), key, key_len);
	/* Zero any pad bytes between the key and the 4-byte boundary. */
	memset(p + sizeof(kl16) + key_len, 0,
	       entry_size - sizeof(kl16) - key_len);

	kv_req->list_off += entry_size;
	return true;
}

static void
nvmf_kvdev_list_done(void *cb_arg, int status, uint32_t num_keys)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	struct spdk_nvmf_request *req = kv_req->req;
	uint32_t nrk = num_keys;

	if (status == SPDK_KVDEV_IO_STATUS_SUCCESS) {
		/* Patch the Number of Returned Keys header now that iteration is
		 * done, then scatter the assembled structure back if we built it in
		 * a bounce buffer. */
		memcpy(kv_req->list_buf, &nrk, sizeof(nrk));

		if (kv_req->bounce != NULL) {
			spdk_copy_buf_to_iovs(req->iov, req->iovcnt, kv_req->bounce,
					      kv_req->list_off);
		}
	}

	/* value_len is unused for List (status is mapped to the CQE by
	 * nvmf_kvdev_complete; SUCCESS leaves cdw0 == 0). */
	nvmf_kvdev_complete(kv_req, status, 0);
}

/*
 * Obtain a contiguous data pointer for the request payload. When the request
 * has a single iov we can use it directly; otherwise we allocate a bounce
 * buffer (gathered for store, scattered after retrieve).
 */
static void *
nvmf_kvdev_get_contig_buf(struct spdk_nvmf_request *req, bool gather,
			  struct nvmf_kvdev_request *kv_req)
{
	void *buf;

	if (req->iovcnt == 1) {
		return req->iov[0].iov_base;
	}

	buf = malloc(req->length ? req->length : 1);
	if (buf == NULL) {
		return NULL;
	}

	if (gather) {
		spdk_copy_iovs_to_buf(buf, req->length, req->iov, req->iovcnt);
	}

	kv_req->bounce = buf;
	return buf;
}

int
nvmf_kvdev_ctrlr_process_io_cmd(struct spdk_nvmf_ns *ns, struct spdk_io_channel *ch,
				struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct nvmf_kvdev_request *kv_req;
	uint8_t key[SPDK_KVDEV_KEY_MAX_LEN];
	uint8_t key_len;
	uint32_t xfer_len;
	void *data;
	int rc;

	nvmf_kvdev_decode_key(cmd, key, &key_len);
	/* List uses the key as a START POSITION, where length 0 means "from the
	 * beginning"; every other KV command requires a real 1..16 byte key. */
	if (key_len > SPDK_KVDEV_KEY_MAX_LEN ||
	    (key_len < SPDK_KVDEV_KEY_MIN_LEN && cmd->opc != SPDK_NVME_OPC_KV_LIST)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_KEY_SIZE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	kv_req = calloc(1, sizeof(*kv_req));
	if (kv_req == NULL) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	kv_req->req = req;

	/* CDW10 holds the value size (store) or host buffer size (retrieve). */
	xfer_len = cmd->cdw10_bits.kv.vsize;
	if (spdk_unlikely(xfer_len > req->length)) {
		SPDK_ERRLOG("KV transfer length %u exceeds request length %u\n",
			    xfer_len, req->length);
		free(kv_req);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	kv_req->xfer_len = xfer_len;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_KV_STORE: {
		struct spdk_kvdev_store_opts opts;

		spdk_kvdev_store_opts_init(&opts, sizeof(opts));
		/* Store Option bits live in CDW11 Request Options (ro), per the KV
		 * Command Set spec (CDW11 bit 8 == ro bit 0, bit 9 == ro bit 1):
		 *   - "Don't store if key does NOT exist"  => Store-If-Key-Exists (SIKE)
		 *   - "Don't store if key DOES exist"       => Store-If-No-Key-Exists (SINKE)
		 */
		if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS) {
			opts.flags |= SPDK_KVDEV_STORE_FLAG_SIKE;
		}
		if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS) {
			opts.flags |= SPDK_KVDEV_STORE_FLAG_SINKE;
		}
		/* Vendor TTL extension (ADR-0003): when the TTL Valid Store Option
		 * bit is set, CDW12 carries a TTL in seconds. Pass it through to
		 * the backend, which persists but does not enforce it. */
		if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_TTL_VALID) {
			opts.flags |= SPDK_KVDEV_STORE_F_TTL;
			opts.ttl = cmd->cdw12_bits.kv_store.ttl;
		}

		data = nvmf_kvdev_get_contig_buf(req, true, kv_req);
		if (data == NULL) {
			goto err_nomem;
		}
		rc = spdk_kvdev_store(ns->kvdev_desc, ch, key, key_len, data, xfer_len,
				      &opts, nvmf_kvdev_store_done, kv_req);
		break;
	}
	case SPDK_NVME_OPC_KV_RETRIEVE:
		data = nvmf_kvdev_get_contig_buf(req, false, kv_req);
		if (data == NULL) {
			goto err_nomem;
		}
		rc = spdk_kvdev_retrieve(ns->kvdev_desc, ch, key, key_len, data, xfer_len,
					 nvmf_kvdev_retrieve_done, kv_req);
		break;
	case SPDK_NVME_OPC_KV_DELETE:
		/* No-data command: no host payload, so bypass the bounce/value path. */
		rc = spdk_kvdev_delete(ns->kvdev_desc, ch, key, key_len,
				       nvmf_kvdev_simple_done, kv_req);
		break;
	case SPDK_NVME_OPC_KV_EXIST:
		/* No-data command: no host payload, so bypass the bounce/value path. */
		rc = spdk_kvdev_exist(ns->kvdev_desc, ch, key, key_len,
				      nvmf_kvdev_simple_done, kv_req);
		break;
	case SPDK_NVME_OPC_KV_LIST: {
		const void *start_key = key_len > 0 ? key : NULL;

		/* The host buffer (CDW10) must hold at least the 4-byte NRK header. */
		if (xfer_len < NVMF_KV_LIST_NRK_SIZE) {
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Assemble the return structure in a contiguous buffer (bounce when
		 * the payload is multi-iov). list_off starts past the NRK header,
		 * which nvmf_kvdev_list_done() patches once the count is known. */
		data = nvmf_kvdev_get_contig_buf(req, false, kv_req);
		if (data == NULL) {
			goto err_nomem;
		}
		kv_req->list_buf = data;
		kv_req->list_cap = xfer_len;
		kv_req->list_off = NVMF_KV_LIST_NRK_SIZE;

		rc = spdk_kvdev_list(ns->kvdev_desc, ch, start_key, key_len,
				     nvmf_kvdev_list_iter, kv_req,
				     nvmf_kvdev_list_done, kv_req);
		break;
	}
	default:
		SPDK_ERRLOG("Unsupported KV opcode 0x%02x\n", cmd->opc);
		free(kv_req);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (rc != 0) {
		/* The kvdev could not even accept the request; no completion will
		 * fire, so complete it here. */
		free(kv_req->bounce);
		free(kv_req);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* The in-memory kvdev completes synchronously, but the contract is async:
	 * the completion callback has already (or will have) completed the
	 * request via spdk_nvmf_request_complete(). */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

err_nomem:
	free(kv_req->bounce);
	free(kv_req);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}
