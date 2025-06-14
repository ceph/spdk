/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/assert.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"
#include "spdk/uuid.h"
#include "spdk/json.h"
#include "spdk/file.h"
#include "spdk/bit_array.h"
#include "spdk/bdev.h"

#define __SPDK_BDEV_MODULE_ONLY
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk_internal/utf.h"
#include "spdk_internal/usdt.h"
#include "nvmf_reservation.h"

bool  ns_rbd_is_ptpl_capable (const struct spdk_nvmf_ns *ns);

int  ns_rdb_update(const struct spdk_nvmf_ns *ns, const struct spdk_nvmf_reservation_info *info);

int  ns_rdb_load (const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info);

//int  ns_rbd_reservation_update_json (struct spdk_bdev *bdev , struct spdk_json_write_ctx **ctx)

//int  ns_rbd_reservation_load_json(struct spdk_bdev *bdev, void **json, int *json_size)

bool  ns_rbd_is_ptpl_capable (const struct spdk_nvmf_ns *ns) {
	return true;
}

int  ns_rdb_update(const struct spdk_nvmf_ns *ns, const struct spdk_nvmf_reservation_info *info) {
	return 0;
}

int  ns_rdb_load (const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info) {
	return 0;
}


static struct spdk_nvmf_ns_reservation_ops g_rbd_ops = {
	.is_ptpl_capable = ns_rbd_is_ptpl_capable,
	.update = ns_rdb_update,
	.load = ns_rdb_load,
};


void spdk_set_rbd_reservation_ops_set(void) {
	spdk_nvmf_set_custom_ns_reservation_ops(&g_rbd_ops);
}
