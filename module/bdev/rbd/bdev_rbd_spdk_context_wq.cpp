/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025,2026 IBM, Inc.
 *   All rights reserved.
 */

#include <rbd/asio/ContextWQ.hpp>
#include <rados/librados.hpp>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bdev_rbd_spdk_context_wq.h"

extern "C" {
#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk_internal/event.h"
}

/**
 * Encapsulates the set of reactor threads for round-robin assignment.
 */
class ReactorThreadPool {
public:
  /** Ensures reactor list exists (lock-free one-time init via CAS). */
  static void ensure_discovered() {
    if (g_reactor_list.load(std::memory_order_acquire) != nullptr) {
      return;
    }
    auto *p = new std::vector<struct spdk_thread *>();
    discover_into(p);
    std::vector<struct spdk_thread *> *expected = nullptr;
    if (!g_reactor_list.compare_exchange_strong(expected, p, std::memory_order_release)) {
      delete p;
    }
  }

  static struct spdk_thread *get_next() {
    std::vector<struct spdk_thread *> *list =
        g_reactor_list.load(std::memory_order_acquire);
    if (list == nullptr || list->empty()) {
      SPDK_ERRLOG("bdev_rbd: reactor thread pool is empty, no reactor threads available for SpdkContextWQ\n");
      return NULL;
    }
    size_t n = list->size();
    size_t idx = g_reactor_next.fetch_add(1, std::memory_order_relaxed) % n;
    struct spdk_thread *t = (*list)[idx];
    const char *name = spdk_thread_get_name(t);
    SPDK_NOTICELOG("bdev_rbd: next reactor thread=%p (id=%lu, name=%s, index=%zu/%zu)\n",
                   t, spdk_thread_get_id(t), name ? name : "NULL", idx, n);
    return t;
  }

private:
  static void discover_into(std::vector<struct spdk_thread *> *vec) {
    uint32_t lcore;
    SPDK_ENV_FOREACH_CORE(lcore) {
      struct spdk_reactor *reactor = spdk_reactor_get(lcore);
      if (reactor == NULL || !reactor->flags.is_valid) {
        continue;
      }
      if (reactor->thread_count == 0) {
        continue;
      }
      struct spdk_lw_thread *lw_thread = TAILQ_FIRST(&reactor->threads);
      if (lw_thread == NULL) {
        continue;
      }
      struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);
      if (thread == NULL || spdk_thread_is_app_thread(thread)) {
        continue;
      }
      const char *name = spdk_thread_get_name(thread);
      SPDK_NOTICELOG("bdev_rbd: discovered reactor thread=%p (id=%lu, name=%s, index=%zu)\n",
                     thread, spdk_thread_get_id(thread), name ? name : "NULL", vec->size());
      vec->push_back(thread);
    }
    if (!vec->empty()) {
      SPDK_NOTICELOG("bdev_rbd: reactor thread pool: discovered %zu reactor(s) for round-robin\n",
                     vec->size());
    }
  }

  static std::atomic<std::vector<struct spdk_thread *> *> g_reactor_list;
  static std::atomic<uint32_t> g_reactor_next;
};

std::atomic<std::vector<struct spdk_thread *> *> ReactorThreadPool::g_reactor_list{nullptr};
std::atomic<uint32_t> ReactorThreadPool::g_reactor_next{0};

/*
 * Lock-free work delivery to a reactor thread.
 *
 */
namespace {

struct ProducerPool;

struct SpdkFnMsg {
  librbd::asio::ContextWQ::Work fn;
  SpdkFnMsg *next = nullptr;
  ProducerPool *owner = nullptr;
};

/* Per-producer recycling pool for SpdkFnMsg: a Treiber stack */
struct ProducerPool {
  std::atomic<SpdkFnMsg *> free{nullptr};
};

thread_local ProducerPool *t_producer_pool = nullptr;

static ProducerPool *
get_producer_pool()
{
  if (t_producer_pool == nullptr) {
    t_producer_pool = new ProducerPool();
  }
  return t_producer_pool;
}

/* Allocate a message on the producer thread, recycling where possible */
static SpdkFnMsg *
pool_alloc(librbd::asio::ContextWQ::Work fn)
{
  ProducerPool *p = get_producer_pool();
  SpdkFnMsg *m = p->free.load(std::memory_order_acquire);
  while (m != nullptr &&
         !p->free.compare_exchange_weak(m, m->next,
             std::memory_order_acquire, std::memory_order_acquire)) {
  }
  if (m == nullptr) {
    m = new SpdkFnMsg();
  }
  m->fn = std::move(fn);
  m->owner = p;
  return m;
}

/* Return a message from the consumer side to its owning pool. */
static void
pool_free(SpdkFnMsg *m)
{
  m->fn = nullptr;

  ProducerPool *p = m->owner;
  SpdkFnMsg *old = p->free.load(std::memory_order_relaxed);
  do {
    m->next = old;
  } while (!p->free.compare_exchange_weak(
             old, m, std::memory_order_release, std::memory_order_relaxed));
}

struct ReactorDispatch {
  struct spdk_thread *reactor = nullptr;
  struct spdk_ring   *work_ring = nullptr;   /* MP/SC: SpdkFnMsg* to execute */
  struct spdk_poller *poller = nullptr;      /* registered on the reactor */
};

constexpr size_t SPDK_WQ_RING_SIZE = 65536;  /* must be power of two */
constexpr size_t SPDK_WQ_POLL_BATCH = 64;

/* Reactor-context poller: drain the work ring and run each functor inline. */
static int
reactor_dispatch_poll(void *arg)
{
  auto *rd = static_cast<ReactorDispatch *>(arg);
  void *items[SPDK_WQ_POLL_BATCH];
  size_t n = spdk_ring_dequeue(rd->work_ring, items, SPDK_WQ_POLL_BATCH);

  for (size_t i = 0; i < n; i++) {
    auto *m = static_cast<SpdkFnMsg *>(items[i]);
    m->fn();
    pool_free(m);
  }

  return n > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/* Pollers belong to the thread that registers them, so this runs on reactor. */
static void
reactor_register_poller(void *arg)
{
  auto *rd = static_cast<ReactorDispatch *>(arg);
  rd->poller = SPDK_POLLER_REGISTER(reactor_dispatch_poll, rd, 0);
  SPDK_NOTICELOG("bdev_rbd: registered SpdkContextWQ dispatch poller on reactor=%p (id=%lu)\n",
                 rd->reactor, spdk_thread_get_id(rd->reactor));
}

/* Fallback path (ring full / no dispatch ctx): legacy spdk_thread_send_msg. */
static void
reactor_fallback_handler(void *arg)
{
  auto *m = static_cast<SpdkFnMsg *>(arg);
  m->fn();
  pool_free(m);
}

static std::mutex g_dispatch_mutex;
static std::unordered_map<struct spdk_thread *, ReactorDispatch *> g_dispatch_map;

/*
 * Get (or lazily create) the dispatch context for a reactor. Reactors live for
 * the lifetime of the process, so the context and its poller are never torn
 * down, keeps producer/consumer teardown race-free. Called off the hot
 * path (image open), so the mutex is fine.
 */
static ReactorDispatch *
get_reactor_dispatch(struct spdk_thread *reactor)
{
  std::lock_guard<std::mutex> lk(g_dispatch_mutex);

  auto it = g_dispatch_map.find(reactor);
  if (it != g_dispatch_map.end()) {
    return it->second;
  }

  auto *rd = new ReactorDispatch();
  rd->reactor = reactor;
  rd->work_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, SPDK_WQ_RING_SIZE,
                                   SPDK_ENV_NUMA_ID_ANY);
  if (rd->work_ring == nullptr) {
    SPDK_ERRLOG("bdev_rbd: failed to create dispatch ring for reactor=%p\n", reactor);
    delete rd;
    return nullptr;
  }

  g_dispatch_map[reactor] = rd;

  /* Register the poller on the reactor thread (one-shot, off the hot path). */
  if (spdk_get_thread() == reactor) {
    reactor_register_poller(rd);
  } else {
    spdk_thread_send_msg(reactor, reactor_register_poller, rd);
  }

  return rd;
}

} // anonymous namespace

namespace librbd {
namespace asio {

SpdkContextWQ::SpdkContextWQ(void* cct, struct spdk_thread* reactor_thread)
  : ContextWQ(cct), m_reactor_thread(reactor_thread), m_dispatch(nullptr) {
  assert(reactor_thread != nullptr);
  m_dispatch = get_reactor_dispatch(reactor_thread);
  if (m_dispatch == nullptr) {
    SPDK_ERRLOG("SpdkContextWQ: failed to obtain reactor dispatch context for reactor=%p; "
                "falling back to spdk_thread_send_msg\n", reactor_thread);
  }
}

SpdkContextWQ::~SpdkContextWQ() {
  // Set shutdown flag to reject new operations
  m_shutdown.store(true, std::memory_order_release);

  // Wait for all pending messages to complete
  drain();

  // Verify all messages are processed
  uint64_t queued = m_queued_ops.load(std::memory_order_acquire);
  if (queued > 0) {
    SPDK_ERRLOG("SpdkContextWQ::~SpdkContextWQ: Warning: %lu operations still pending during destruction\n", queued);
  }
}

void SpdkContextWQ::send_fn(Work fn) {
  auto *msg = pool_alloc(std::move(fn));
  auto *rd = static_cast<ReactorDispatch *>(m_dispatch);

  // Fast path: lock-free enqueue into the reactor's MP/SC work ring. The reactor
  // poller drains it.
  if (rd != nullptr) {
    void *item = msg;
    if (spdk_ring_enqueue(rd->work_ring, &item, 1, NULL) == 1) {
      return;
    }
    // Ring full: fall through to the legacy path to guarantee forward progress.
  }

  // Fallback: legacy cross-thread message. Still correct, just slower.
  int rc = spdk_thread_send_msg(m_reactor_thread, reactor_fallback_handler, msg);
  if (rc != 0) {
    pool_free(msg);
    SPDK_ERRLOG("SpdkContextWQ::send_fn: fallback spdk_thread_send_msg failed rc=%d\n", rc);
  }
}

void SpdkContextWQ::post(Work fn) {
  send_fn(std::move(fn));
}

void SpdkContextWQ::dispatch(Work fn) {
  if (spdk_get_thread() == m_reactor_thread) {
    fn();
  } else {
    send_fn(std::move(fn));
  }
}

void SpdkContextWQ::post_serial(Work fn) {
  send_fn(std::move(fn));
}

void SpdkContextWQ::dispatch_serial(Work fn) {
  dispatch(std::move(fn));
}

void SpdkContextWQ::drain() {
  // Wait for all pending messages to be processed.
  // Note: This relies on the SPDK reactor thread to be actively polling.
  // TODO: conf parameter, non busy wait implementation
  const int max_iterations = 100000;  // 10 seconds at 100us per iteration
  int iterations = 0;

  // Wait for all queued operations to complete
  while (m_queued_ops.load(std::memory_order_acquire) > 0 &&
         iterations < max_iterations) {
    // Yield to allow SPDK reactor thread to process messages
    spdk_delay_us(100);
    ++iterations;
  }

  uint64_t queued = m_queued_ops.load(std::memory_order_acquire);
  if (queued > 0) {
    SPDK_ERRLOG("SpdkContextWQ::drain: Incomplete drain - queued_ops=%lu after %d iterations\n",
                queued, iterations);
  }
}

} // namespace asio
} // namespace librbd

// C API implementation
extern "C" {

struct bdev_rbd_spdk_context_wq* bdev_rbd_spdk_context_wq_create_from_ioctx(rados_ioctx_t io_ctx, struct spdk_thread* reactor_thread)
{
  if (io_ctx == NULL || reactor_thread == NULL) {
    return NULL;
  }

  // Convert rados_ioctx_t to librados::IoCtx to get CephContext
  librados::IoCtx ioctx;
  librados::IoCtx::from_rados_ioctx_t(io_ctx, ioctx);
  void* cct_ptr = ioctx.cct();

  if (cct_ptr == NULL) {
    SPDK_ERRLOG("Failed to get CephContext from rados_ioctx_t\n");
    return NULL;
  }

  // Create SpdkContextWQ
  uint64_t thread_id = spdk_thread_get_id(reactor_thread);
  const char *thread_name = spdk_thread_get_name(reactor_thread);
  try {
    auto wq = new librbd::asio::SpdkContextWQ(cct_ptr, reactor_thread);
    // Cast to opaque struct pointer for type safety
    struct bdev_rbd_spdk_context_wq* result = reinterpret_cast<struct bdev_rbd_spdk_context_wq*>(wq);
    SPDK_NOTICELOG("bdev_rbd_spdk_context_wq_create_from_ioctx: Successfully created SpdkContextWQ=%p with reactor thread=%p (id=%lu, name=%s)\n",
                   result, reactor_thread, thread_id, thread_name ? thread_name : "NULL");
    return result;
  } catch (...) {
    SPDK_ERRLOG("bdev_rbd_spdk_context_wq_create_from_ioctx: Failed to create SpdkContextWQ with reactor thread=%p (id=%lu, name=%s)\n",
                reactor_thread, thread_id, thread_name ? thread_name : "NULL");
    return NULL;
  }
}

void bdev_rbd_spdk_context_wq_destroy(struct bdev_rbd_spdk_context_wq* context_wq)
{
  if (context_wq == NULL) {
    return;
  }

  // Cast back to SpdkContextWQ and delete
  auto wq = reinterpret_cast<librbd::asio::SpdkContextWQ*>(context_wq);
  delete wq;
}

struct spdk_thread* bdev_rbd_find_reactor_thread(void)
{
  ReactorThreadPool::ensure_discovered();
  return ReactorThreadPool::get_next();
}

} // extern "C"
