#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = kvdev.c
LIBNAME = kvdev

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_kvdev.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
