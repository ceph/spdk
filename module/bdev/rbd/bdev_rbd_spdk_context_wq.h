/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025,2026 IBM, Inc.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_RBD_SPDK_CONTEXT_WQ_H
#define SPDK_BDEV_RBD_SPDK_CONTEXT_WQ_H

// Forward declaration for SPDK thread
struct spdk_thread;

// Forward declaration for rbd_image_t (defined in <rbd/librbd.h>)
// We use void* here to avoid including librbd.h in the header
#ifndef rbd_image_t
typedef void* rbd_image_t;
#endif

// Forward declaration for rados_ioctx_t (defined in <rados/librados.h>)
#ifndef rados_ioctx_t
typedef void* rados_ioctx_t;
#endif

// Opaque type for SpdkContextWQ - provides type safety in C code
// The actual implementation is C++ and is hidden behind this opaque pointer
struct bdev_rbd_spdk_context_wq;

// C API for creating SpdkContextWQ from C code (bdev_rbd.c)
// These declarations are available to both C and C++ code
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a SpdkContextWQ from rados_ioctx_t and SPDK reactor thread.
 * The returned pointer must be freed by calling bdev_rbd_spdk_context_wq_destroy().
 *
 * @param io_ctx RADOS I/O context (rados_ioctx_t)
 * @param reactor_thread SPDK reactor thread
 * @return Pointer to SpdkContextWQ, or NULL on error
 */
struct bdev_rbd_spdk_context_wq* bdev_rbd_spdk_context_wq_create_from_ioctx(rados_ioctx_t io_ctx, struct spdk_thread* reactor_thread);

/**
 * Destroy a SpdkContextWQ created by bdev_rbd_spdk_context_wq_create_from_ioctx().
 *
 * @param context_wq Pointer to SpdkContextWQ
 */
void bdev_rbd_spdk_context_wq_destroy(struct bdev_rbd_spdk_context_wq* context_wq);

/**
 * Return the next reactor thread for SpdkContextWQ (round-robin).
 * Lazy-initializes the reactor list on first call (lock-free, atomic CAS).
 * Each call returns a different reactor so RBD images are balanced across reactors.
 * Returns NULL if no reactor threads were discovered (error is logged).
 */
struct spdk_thread* bdev_rbd_find_reactor_thread(void);

#ifdef __cplusplus
}

// C++ class definition - only available when compiling C++ code
#include <rbd/asio/ContextWQ.hpp>

namespace librbd {
namespace asio {

/**
 * ContextWQ implementation that schedules work on SPDK reactor threads
 */
class SpdkContextWQ : public ContextWQ {
public:
  explicit SpdkContextWQ(void* cct, struct spdk_thread* reactor_thread);
  ~SpdkContextWQ();

  void drain() override;

  void post(Work fn) override;
  void dispatch(Work fn) override;
  void post_serial(Work fn) override;
  void dispatch_serial(Work fn) override;

private:
  struct spdk_thread* m_reactor_thread;

  // Per-reactor dispatch context (ReactorDispatch*), created lazily and shared
  // by all SpdkContextWQ instances bound to the same reactor. Opaque here;
  // defined in the .cpp. Work is delivered to the reactor through a lock-free
  // MP/SC ring drained by an SPDK poller, which avoids spdk_thread_send_msg()
  // and therefore the shared g_spdk_msg_mempool and its rte_pause lock spinning.
  void* m_dispatch;

  void send_fn(Work fn);
};

} // namespace asio
} // namespace librbd

#endif // __cplusplus

#endif /* SPDK_BDEV_RBD_SPDK_CONTEXT_WQ_H */
