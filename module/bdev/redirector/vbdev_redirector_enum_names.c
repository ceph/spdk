/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Enables relevant enum type values to be converted into their names for display
 * and logging.
 */

#include "spdk/bdev_module.h"
#include "vbdev_redirector_enum_names.h"
#include "vbdev_redirector_types.h"

const char *
spdk_bdev_io_type_enum_name[SPDK_BDEV_NUM_IO_TYPES + 1] = {
	[SPDK_BDEV_IO_TYPE_INVALID] = "SPDK_BDEV_IO_TYPE_INVALID",
	[SPDK_BDEV_IO_TYPE_READ] = "SPDK_BDEV_IO_TYPE_READ",
	[SPDK_BDEV_IO_TYPE_WRITE] = "SPDK_BDEV_IO_TYPE_WRITE",
	[SPDK_BDEV_IO_TYPE_UNMAP] = "SPDK_BDEV_IO_TYPE_UNMAP",
	[SPDK_BDEV_IO_TYPE_FLUSH] = "SPDK_BDEV_IO_TYPE_FLUSH",
	[SPDK_BDEV_IO_TYPE_RESET] = "SPDK_BDEV_IO_TYPE_RESET",
	[SPDK_BDEV_IO_TYPE_NVME_ADMIN] = "SPDK_BDEV_IO_TYPE_NVME_ADMIN",
	[SPDK_BDEV_IO_TYPE_NVME_IO] = "SPDK_BDEV_IO_TYPE_NVME_IO",
	[SPDK_BDEV_IO_TYPE_NVME_IO_MD] = "SPDK_BDEV_IO_TYPE_NVME_IO_MD",
	[SPDK_BDEV_IO_TYPE_WRITE_ZEROES] = "SPDK_BDEV_IO_TYPE_WRITE_ZEROES",
	[SPDK_BDEV_IO_TYPE_ZCOPY] = "SPDK_BDEV_IO_TYPE_ZCOPY",
	[SPDK_BDEV_NUM_IO_TYPES] = "SPDK_BDEV_NUM_IO_TYPES"
};

const char *
spdk_bdev_io_type_name[SPDK_BDEV_NUM_IO_TYPES + 1] = {
	[SPDK_BDEV_IO_TYPE_INVALID] = "INVALID",
	[SPDK_BDEV_IO_TYPE_READ] = "READ",
	[SPDK_BDEV_IO_TYPE_WRITE] = "WRITE",
	[SPDK_BDEV_IO_TYPE_UNMAP] = "UNMAP",
	[SPDK_BDEV_IO_TYPE_FLUSH] = "FLUSH",
	[SPDK_BDEV_IO_TYPE_RESET] = "RESET",
	[SPDK_BDEV_IO_TYPE_NVME_ADMIN] = "NVME_ADMIN",
	[SPDK_BDEV_IO_TYPE_NVME_IO] = "NVME_IO",
	[SPDK_BDEV_IO_TYPE_NVME_IO_MD] = "NVME_IO_MD",
	[SPDK_BDEV_IO_TYPE_WRITE_ZEROES] = "WRITE_ZEROES",
	[SPDK_BDEV_IO_TYPE_ZCOPY] = "ZCOPY",
	[SPDK_BDEV_NUM_IO_TYPES] = "SPDK_BDEV_NUM_IO_TYPES"
};

const char *spdk_nvme_status_code_type_name[256] = {
	[SPDK_NVME_SCT_GENERIC] = "SCT_GENERIC",
	[SPDK_NVME_SCT_COMMAND_SPECIFIC] = "SCT_COMMAND_SPECIFIC",
	[SPDK_NVME_SCT_MEDIA_ERROR] = "SCT_MEDIA_ERROR",
	[SPDK_NVME_SCT_PATH] = "SCT_PATH",
	/* 0x4-0x6 - reserved */
	[0x04] = "SCT_RESERVED_0x04",
	[0x05] = "SCT_RESERVED_0x05",
	[0x06] =  "SCT_RESERVED_0x06",
	[SPDK_NVME_SCT_VENDOR_SPECIFIC] = "SCT_VENDOR_SPECIFIC"
};

const char *spdk_nvme_generic_command_status_code_name[256] = {
	[SPDK_NVME_SC_SUCCESS] =			"SC_SUCCESS",
	[SPDK_NVME_SC_INVALID_OPCODE] =			"SC_INVALID_OPCODE",
	[SPDK_NVME_SC_INVALID_FIELD] =			"SC_INVALID_FIELD",
	[SPDK_NVME_SC_COMMAND_ID_CONFLICT] =		"SC_COMMAND_ID_CONFLICT",
	[SPDK_NVME_SC_DATA_TRANSFER_ERROR] =		"SC_DATA_TRANSFER_ERROR",
	[SPDK_NVME_SC_ABORTED_POWER_LOSS] =		"SC_ABORTED_POWER_LOSS",
	[SPDK_NVME_SC_INTERNAL_DEVICE_ERROR] =		"SC_INTERNAL_DEVICE_ERROR",
	[SPDK_NVME_SC_ABORTED_BY_REQUEST] =		"SC_ABORTED_BY_REQUEST",
	[SPDK_NVME_SC_ABORTED_SQ_DELETION] =		"SC_ABORTED_SQ_DELETION",
	[SPDK_NVME_SC_ABORTED_FAILED_FUSED] =		"SC_ABORTED_FAILED_FUSED",
	[SPDK_NVME_SC_ABORTED_MISSING_FUSED] =		"SC_ABORTED_MISSING_FUSED",
	[SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT] =	"SC_INVALID_NAMESPACE_OR_FORMAT",
	[SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR] =		"SC_COMMAND_SEQUENCE_ERROR",
	[SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR] =	"SC_INVALID_SGL_SEG_DESCRIPTOR",
	[SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS] =	"SC_INVALID_NUM_SGL_DESCIRPTORS",
	[SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID] =	"SC_DATA_SGL_LENGTH_INVALID",
	[SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID] =	"SC_METADATA_SGL_LENGTH_INVALID",
	[SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID] =	"SC_SGL_DESCRIPTOR_TYPE_INVALID",
	[SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF] =	"SC_INVALID_CONTROLLER_MEM_BUF",
	[SPDK_NVME_SC_INVALID_PRP_OFFSET] =		"SC_INVALID_PRP_OFFSET",
	[SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED] =	"SC_ATOMIC_WRITE_UNIT_EXCEEDED",
	[SPDK_NVME_SC_OPERATION_DENIED] =		"SC_OPERATION_DENIED",
	[SPDK_NVME_SC_INVALID_SGL_OFFSET] =		"SC_INVALID_SGL_OFFSET",

	/* 0x17 - reserved */				/* 0x17 - reserved */
	[0x17] =					"SC_RESERVED_0x17",

	[SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT] =	"SC_HOSTID_INCONSISTENT_FORMAT",
	[SPDK_NVME_SC_KEEP_ALIVE_EXPIRED] =		"SC_KEEP_ALIVE_EXPIRED",
	[SPDK_NVME_SC_KEEP_ALIVE_INVALID] =		"SC_KEEP_ALIVE_INVALID",
	[SPDK_NVME_SC_ABORTED_PREEMPT] =		"SC_ABORTED_PREEMPT",
	[SPDK_NVME_SC_SANITIZE_FAILED] =		"SC_SANITIZE_FAILED",
	[SPDK_NVME_SC_SANITIZE_IN_PROGRESS] =		"SC_SANITIZE_IN_PROGRESS",
	[SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID] = "SC_SGL_DATA_BLOCK_GRANULARITY_INVALID",
	[SPDK_NVME_SC_COMMAND_INVALID_IN_CMB] =		"SC_COMMAND_INVALID_IN_CMB",

	[SPDK_NVME_SC_LBA_OUT_OF_RANGE] =		"SC_LBA_OUT_OF_RANGE",
	[SPDK_NVME_SC_CAPACITY_EXCEEDED] =		"SC_CAPACITY_EXCEEDED",
	[SPDK_NVME_SC_NAMESPACE_NOT_READY] =		"SC_NAMESPACE_NOT_READY",
	[SPDK_NVME_SC_RESERVATION_CONFLICT] =           "SC_RESERVATION_CONFLICT",
	[SPDK_NVME_SC_FORMAT_IN_PROGRESS] =             "SC_FORMAT_IN_PROGRESS"
};

extern const char *get_spdk_nvme_status_code_type_name(int sct)
{
	const char *sct_name = spdk_nvme_status_code_type_name[sct];

	if (sct_name) {
		return sct_name;
	} else {
		return "[UNKNOWN]";
	}
}

static const char *get_spdk_nvme_generic_command_status_code_name(int generic_sc)
{
	const char *generic_sc_name = spdk_nvme_generic_command_status_code_name[generic_sc];

	if (generic_sc_name) {
		return generic_sc_name;
	} else {
		return "[UNKNOWN]";
	}
}

extern const char *get_spdk_nvme_command_status_code_name(int sct, int sc)
{
	switch (sct) {
	case SPDK_NVME_SCT_GENERIC:
		return get_spdk_nvme_generic_command_status_code_name(sc);
		break;
	default:
		return "[UNKNOWN]";
	}
}

const char *
rd_hint_type_name[256] = {
	[RD_HINT_TYPE_NONE] =			"RD_HINT_TYPE_NONE",
	[RD_HINT_TYPE_SIMPLE_NQN] =		"RD_HINT_TYPE_SIMPLE_NQN",
	[RD_HINT_TYPE_SIMPLE_NQN_NS] =		"RD_HINT_TYPE_SIMPLE_NQN_NS",
	[RD_HINT_TYPE_SIMPLE_NQN_ALT] =		"RD_HINT_TYPE_SIMPLE_NQN_ALT",
	[RD_HINT_TYPE_SIMPLE_NQN_TABLE] =	"RD_HINT_TYPE_SIMPLE_NQN_TABLE",
	[RD_HINT_TYPE_STRIPE_NQN] =		"RD_HINT_TYPE_STRIPE_NQN",
	[RD_HINT_TYPE_STRIPE_NQN_NS] =		"RD_HINT_TYPE_STRIPE_NQN_NS",
	[RD_HINT_TYPE_HASH_NQN_TABLE] =		"RD_HINT_TYPE_HASH_NQN_TABLE",
	[RD_HINT_TYPE_DIFF_NQN_NS] =		"RD_HINT_TYPE_DIFF_NQN_NS",
	[RD_HINT_TYPE_DIFF_HASH_NQN_TABLE] =	"RD_HINT_TYPE_DIFF_HASH_NQN_TABLE",
	[RD_HINT_TYPE_VENDOR_SPEC_START] =	"RD_HINT_TYPE_VENDOR_SPEC_START",
	[RD_HINT_TYPE_VENDOR_SPEC_END] =	"RD_HINT_TYPE_VENDOR_SPEC_END"
};

const char *
rd_hash_fn_id_name[RD_HASH_FN_ID_LAST] = {
	[RD_HASH_FN_ID_NONE] = "none",
	[RD_HASH_FN_ID_CEPH_RJENKINS] = "ceph_rjenkins",
	[RD_HASH_FN_ID_NULL] = "null"
};
