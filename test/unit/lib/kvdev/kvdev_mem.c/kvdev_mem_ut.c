/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"

/*
 * The in-memory kvdev module depends on the kvdev core library and the
 * spdk_thread io_channel machinery. We pull in the kvdev core source so the
 * module's spdk_kvdev_register()/spdk_kvdev_get_by_name() calls resolve, and
 * ut_multithread.c provides a real (single) spdk_thread so the io_channel
 * helpers work.
 */
#include "kvdev/kvdev.c"
#include "kvdev/mem/kvdev_mem.c"

#include "common/lib/ut_multithread.c"

static int g_status;
static uint32_t g_value_len;
static bool g_completed;

static void
kv_op_cb(void *cb_arg, int status, uint32_t value_len)
{
	g_status = status;
	g_value_len = value_len;
	g_completed = true;
}

static struct spdk_kvdev *
create_test_kvdev(const char *name, uint32_t max_value_len, uint32_t max_num_keys)
{
	struct kvdev_mem_opts opts = {};
	struct spdk_kvdev *kvdev = NULL;
	int rc;

	opts.name = name;
	opts.max_value_len = max_value_len;
	opts.max_num_keys = max_num_keys;

	rc = kvdev_mem_create(&opts, &kvdev);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(kvdev != NULL);

	return kvdev;
}

static void
test_kvdev_mem_store_retrieve(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	const char key[] = "mykey";
	const char value[] = "hello, key-value world";
	char buf[64];
	int rc;

	create_test_kvdev("kv0", 0, 0);

	rc = spdk_kvdev_open("kv0", true, &desc);
	CU_ASSERT(rc == 0);

	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* Store */
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), value, sizeof(value), NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* Retrieve into a large-enough buffer; bytes must round-trip. */
	memset(buf, 0, sizeof(buf));
	g_completed = false;
	rc = spdk_kvdev_retrieve(desc, ch, key, sizeof(key), buf, sizeof(buf), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_value_len == sizeof(value));
	CU_ASSERT(memcmp(buf, value, sizeof(value)) == 0);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();

	rc = kvdev_mem_delete("kv0");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_overwrite(void)
{
	struct spdk_kvdev *kvdev;
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	const char key[] = "k";
	char buf[16];
	int rc;

	kvdev = create_test_kvdev("kv1", 0, 0);
	CU_ASSERT(kvdev != NULL);

	rc = spdk_kvdev_open("kv1", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "AAAA", 4, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);

	/* Overwrite with a different value. */
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "BB", 2, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	memset(buf, 0, sizeof(buf));
	g_completed = false;
	rc = spdk_kvdev_retrieve(desc, ch, key, sizeof(key), buf, sizeof(buf), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_value_len == 2);
	CU_ASSERT(memcmp(buf, "BB", 2) == 0);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv1");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_retrieve_missing(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	const char key[] = "absent";
	char buf[16];
	int rc;

	create_test_kvdev("kv2", 0, 0);

	rc = spdk_kvdev_open("kv2", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	g_completed = false;
	rc = spdk_kvdev_retrieve(desc, ch, key, sizeof(key), buf, sizeof(buf), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv2");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_retrieve_truncated(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	const char key[] = "k";
	const char value[] = "0123456789";
	char buf[4];
	int rc;

	create_test_kvdev("kv3", 0, 0);

	rc = spdk_kvdev_open("kv3", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), value, sizeof(value), NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* Retrieve into a too-small buffer: status reports too-small and the
	 * true value length is reported. */
	memset(buf, 0, sizeof(buf));
	g_completed = false;
	rc = spdk_kvdev_retrieve(desc, ch, key, sizeof(key), buf, sizeof(buf), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL);
	CU_ASSERT(g_value_len == sizeof(value));
	CU_ASSERT(memcmp(buf, value, sizeof(buf)) == 0);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv3");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_caps(void)
{
	struct spdk_kvdev *kvdev;
	const struct spdk_kvdev_caps *caps;
	int rc;

	kvdev = create_test_kvdev("kv4", 4096, 100);
	caps = spdk_kvdev_get_caps(kvdev);
	CU_ASSERT(caps->max_key_len == SPDK_KVDEV_KEY_MAX_LEN);
	CU_ASSERT(caps->max_value_len == 4096);
	CU_ASSERT(caps->max_num_keys == 100);

	rc = kvdev_mem_delete("kv4");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_max_keys(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	int rc;

	/* Allow only one key. */
	create_test_kvdev("kv5", 0, 1);
	rc = spdk_kvdev_open("kv5", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	rc = spdk_kvdev_store(desc, ch, "a", 1, "v", 1, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* Second distinct key should be rejected for capacity. */
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, "b", 1, "v", 1, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_NOMEM);

	/* But overwriting the existing key is still allowed. */
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, "a", 1, "w", 1, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv5");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_store_conditional(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_kvdev_store_opts opts;
	const char key[] = "ckey";
	char buf[16];
	int rc;

	create_test_kvdev("kv6", 0, 0);
	rc = spdk_kvdev_open("kv6", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* SIKE (Store-If-Key-Exists) on an absent key must fail KEY_NOT_EXIST. */
	spdk_kvdev_store_opts_init(&opts, sizeof(opts));
	opts.flags = SPDK_KVDEV_STORE_FLAG_SIKE;
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "AAAA", 4, &opts, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	/* SINKE (Store-If-No-Key-Exists) on an absent key must succeed (creates it). */
	spdk_kvdev_store_opts_init(&opts, sizeof(opts));
	opts.flags = SPDK_KVDEV_STORE_FLAG_SINKE;
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "AAAA", 4, &opts, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* SINKE on a now-existing key must fail KEY_EXIST. */
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "BBBB", 4, &opts, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_EXIST);

	/* SIKE on the existing key must now succeed and overwrite. */
	spdk_kvdev_store_opts_init(&opts, sizeof(opts));
	opts.flags = SPDK_KVDEV_STORE_FLAG_SIKE;
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "CC", 2, &opts, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	memset(buf, 0, sizeof(buf));
	g_completed = false;
	rc = spdk_kvdev_retrieve(desc, ch, key, sizeof(key), buf, sizeof(buf), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_value_len == 2);
	CU_ASSERT(memcmp(buf, "CC", 2) == 0);

	/* NULL opts must behave as an unconditional store. */
	g_completed = false;
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "DDDD", 4, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv6");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_delete(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	const char key[] = "delkey";
	char buf[16];
	int rc;

	create_test_kvdev("kv7", 0, 0);
	rc = spdk_kvdev_open("kv7", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* Delete on an absent key must fail KEY_NOT_EXIST. */
	g_completed = false;
	rc = spdk_kvdev_delete(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	/* Store, then delete: delete must succeed. */
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "data", 4, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	g_completed = false;
	rc = spdk_kvdev_delete(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* The key must now be gone: a Retrieve returns KEY_NOT_EXIST. */
	g_completed = false;
	rc = spdk_kvdev_retrieve(desc, ch, key, sizeof(key), buf, sizeof(buf), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	/* A second delete of the now-absent key must again fail KEY_NOT_EXIST. */
	g_completed = false;
	rc = spdk_kvdev_delete(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv7");
	CU_ASSERT(rc == 0);
}

static void
test_kvdev_mem_exist(void)
{
	struct spdk_kvdev_desc *desc;
	struct spdk_io_channel *ch;
	const char key[] = "exkey";
	int rc;

	create_test_kvdev("kv8", 0, 0);
	rc = spdk_kvdev_open("kv8", true, &desc);
	CU_ASSERT(rc == 0);
	ch = spdk_kvdev_get_io_channel(desc);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* Exist on an absent key must report KEY_NOT_EXIST. */
	g_completed = false;
	rc = spdk_kvdev_exist(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	/* After a store, Exist must report SUCCESS (present). */
	rc = spdk_kvdev_store(desc, ch, key, sizeof(key), "v", 1, NULL, kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	g_completed = false;
	rc = spdk_kvdev_exist(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* After a delete, Exist must again report KEY_NOT_EXIST. */
	rc = spdk_kvdev_delete(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_SUCCESS);

	g_completed = false;
	rc = spdk_kvdev_exist(desc, ch, key, sizeof(key), kv_op_cb, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);

	spdk_put_io_channel(ch);
	spdk_kvdev_close(desc);
	poll_threads();
	rc = kvdev_mem_delete("kv8");
	CU_ASSERT(rc == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("kvdev_mem", NULL, NULL);
	CU_ADD_TEST(suite, test_kvdev_mem_store_retrieve);
	CU_ADD_TEST(suite, test_kvdev_mem_overwrite);
	CU_ADD_TEST(suite, test_kvdev_mem_retrieve_missing);
	CU_ADD_TEST(suite, test_kvdev_mem_retrieve_truncated);
	CU_ADD_TEST(suite, test_kvdev_mem_caps);
	CU_ADD_TEST(suite, test_kvdev_mem_max_keys);
	CU_ADD_TEST(suite, test_kvdev_mem_store_conditional);
	CU_ADD_TEST(suite, test_kvdev_mem_delete);
	CU_ADD_TEST(suite, test_kvdev_mem_exist);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	free_threads();

	CU_cleanup_registry();

	return num_failures;
}
