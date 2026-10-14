/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025,2026 IBM, Inc.
 *   All rights reserved.
 */

#include <rbd/asio/ContextWQ.hpp>
#include <rados/librados.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
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
  /*
   * Snapshot reactor threads once after the target is up. For NVMe-oF the
   * reactor/poll-group threads are process-lifetime and are not migrated, so a
   * one-shot walk of reactor->threads is sufficient. A fully synchronized
   * enumeration would be needed if consumers migrate or destroy those threads
   * while SpdkContextWQ is in use.
   */
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
 * Lock-free work delivery to a reactor thread (librbd ContextWQ completions).
 *
 * Submit path (nvmf -> bdev -> rbd_aio_*) stays on submit_td with no ring.
 * This ring is only for msgr/finisher -> reactor delivery of ContextWQ::Work.
 * It parallels SPDK's per-thread messages ring (also MP/SC, default 64K) used
 * by spdk_thread_send_msg(), but keeps payloads on a thread-local SpdkFnMsg
 * freelist so the common path avoids g_spdk_msg_mempool. If our ring is full,
 * we fall back to send_msg (thread->messages + shared mempool).
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
  std::atomic<uint64_t> in_flight{0};
  std::atomic<bool> retiring{false};
};

static void try_reclaim_producer_pool(ProducerPool *p);

struct ProducerPoolTls {
  ProducerPool *pool = nullptr;

  ~ProducerPoolTls() {
    if (pool == nullptr) {
      return;
    }
    /* Thread exit: keep the pool alive until in-flight msgs return, then free. */
    pool->retiring.store(true, std::memory_order_release);
    try_reclaim_producer_pool(pool);
    pool = nullptr;
  }
};

thread_local ProducerPoolTls t_producer_pool;

static ProducerPool *
get_producer_pool()
{
  if (t_producer_pool.pool == nullptr) {
    t_producer_pool.pool = new ProducerPool();
  }
  return t_producer_pool.pool;
}

static void
try_reclaim_producer_pool(ProducerPool *p)
{
  if (p == nullptr || !p->retiring.load(std::memory_order_acquire)) {
    return;
  }
  if (p->in_flight.load(std::memory_order_acquire) != 0) {
    return;
  }
  SpdkFnMsg *m = p->free.exchange(nullptr, std::memory_order_acq_rel);
  while (m != nullptr) {
    SpdkFnMsg *next = m->next;
    delete m;
    m = next;
  }
  delete p;
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
  p->in_flight.fetch_add(1, std::memory_order_relaxed);
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
  p->in_flight.fetch_sub(1, std::memory_order_acq_rel);
  try_reclaim_producer_pool(p);
}

/* All stats counters are relaxed atomics so the stats RPC can sample safely. */
struct ReactorDispatchStats {
  std::atomic<uint64_t> fallback{0};
  std::atomic<uint64_t> deq_items{0};
  std::atomic<uint64_t> deq_batches{0};
  std::atomic<uint64_t> deq_empty{0};
  std::atomic<uint64_t> depth_hwm{0};
};

struct ReactorDispatch {
  struct spdk_thread *reactor = nullptr;
  struct spdk_ring   *work_ring = nullptr;   /* MP/SC: SpdkFnMsg* to execute */
  struct spdk_poller *poller = nullptr;      /* registered on the reactor */
  uint32_t ring_size = 0;
  ReactorDispatchStats stats;
};

static constexpr uint32_t SPDK_WQ_RING_SIZE_DEFAULT = 65536; /* power of two */
static constexpr uint32_t SPDK_WQ_RING_SIZE_MIN = 1024;
static constexpr uint32_t SPDK_WQ_RING_SIZE_MAX = (1u << 20);
static constexpr size_t SPDK_WQ_POLL_BATCH = 64;

/* Protected by g_dispatch_mutex for set; read at ring create under same lock. */
static uint32_t g_wq_ring_size = SPDK_WQ_RING_SIZE_DEFAULT;
static std::atomic<bool> g_wq_fallback_noticed{false};

static bool
is_power_of_two(uint32_t n)
{
  return n != 0 && (n & (n - 1)) == 0;
}

/* Reactor-context poller: drain the work ring and run each functor inline. */
static int
reactor_dispatch_poll(void *arg)
{
  auto *rd = static_cast<ReactorDispatch *>(arg);
  void *items[SPDK_WQ_POLL_BATCH];
  size_t n = spdk_ring_dequeue(rd->work_ring, items, SPDK_WQ_POLL_BATCH);

  uint64_t batches =
      rd->stats.deq_batches.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 0) {
    rd->stats.deq_empty.fetch_add(1, std::memory_order_relaxed);
  } else {
    rd->stats.deq_items.fetch_add(n, std::memory_order_relaxed);
    /* Sparse depth sample: avoid spdk_ring_count every poll. */
    if ((batches & 0x3f) == 0) {
      size_t depth = spdk_ring_count(rd->work_ring);
      uint64_t hwm = rd->stats.depth_hwm.load(std::memory_order_relaxed);
      while (depth > hwm &&
             !rd->stats.depth_hwm.compare_exchange_weak(
                 hwm, depth, std::memory_order_relaxed)) {
      }
    }
  }

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

struct ReactorTeardownCtx {
  ReactorDispatch *rd = nullptr;
  std::atomic<bool> done{false};
};

/* Must run on rd->reactor: unregister poller, drain leftover ring work. */
static void
reactor_teardown_dispatch(void *arg)
{
  auto *ctx = static_cast<ReactorTeardownCtx *>(arg);
  ReactorDispatch *rd = ctx->rd;

  if (rd->poller != nullptr) {
    spdk_poller_unregister(&rd->poller);
  }

  if (rd->work_ring != nullptr) {
    void *items[SPDK_WQ_POLL_BATCH];
    size_t n;
    while ((n = spdk_ring_dequeue(rd->work_ring, items, SPDK_WQ_POLL_BATCH)) > 0) {
      for (size_t i = 0; i < n; i++) {
        auto *m = static_cast<SpdkFnMsg *>(items[i]);
        m->fn();
        pool_free(m);
      }
    }
    spdk_ring_free(rd->work_ring);
    rd->work_ring = nullptr;
  }

  ctx->done.store(true, std::memory_order_release);
}

/* Fallback path (ring full / no dispatch ctx): legacy spdk_thread_send_msg. */
static void
reactor_fallback_handler(void *arg)
{
  auto *m = static_cast<SpdkFnMsg *>(arg);
  m->fn();
  pool_free(m);
}

/* Deliver work to a reactor: lock-free ring first, message fallback. */
static void
dispatch_to_reactor(ReactorDispatch *rd, struct spdk_thread *reactor,
                    librbd::asio::ContextWQ::Work fn)
{
  auto *msg = pool_alloc(std::move(fn));

  // Fast path: lock-free enqueue into the reactor's MP/SC work ring. The reactor
  // poller drains it. No counters on success (multi-producer hot path).
  if (rd != nullptr) {
    void *item = msg;
    if (spdk_ring_enqueue(rd->work_ring, &item, 1, NULL) == 1) {
      return;
    }
    // Ring full: fall through to the legacy path to guarantee forward progress.
    rd->stats.fallback.fetch_add(1, std::memory_order_relaxed);
    if (!g_wq_fallback_noticed.exchange(true, std::memory_order_relaxed)) {
      SPDK_NOTICELOG("bdev_rbd: SpdkContextWQ ring full on reactor=%p (id=%lu); "
                     "falling back to spdk_thread_send_msg (first occurrence)\n",
                     reactor, spdk_thread_get_id(reactor));
    }
  }

  /*
   * Fallback: legacy cross-thread message. Still correct for progress, but can
   * reorder vs work still sitting in the ring (SPDK drains messages before
   * pollers). At default ring size soak runs see fallback ≈ 0; strict
   * serial-through-overflow is a possible follow-up if tiny rings are required.
   */
  int rc = spdk_thread_send_msg(reactor, reactor_fallback_handler, msg);
  if (rc != 0) {
    pool_free(msg);
    SPDK_ERRLOG("bdev_rbd: dispatch_to_reactor: spdk_thread_send_msg failed rc=%d\n", rc);
  }
}

static std::mutex g_dispatch_mutex;
static std::unordered_map<struct spdk_thread *, ReactorDispatch *> g_dispatch_map;

/*
 * Dispatch context of the calling thread. Resolved once per thread so that
 * channel lookups on the submit path take neither a lock nor a map lookup.
 */
thread_local ReactorDispatch *t_self_dispatch = nullptr;

/*
 * Get (or lazily create) the dispatch context for a reactor. Called off the hot
 * path (image open), so the mutex is fine. Torn down from bdev_rbd_wq_fini()
 * on module unload so poll-group threads can exit cleanly.
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
  rd->ring_size = g_wq_ring_size;
  rd->work_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, rd->ring_size,
                                   SPDK_ENV_NUMA_ID_ANY);
  if (rd->work_ring == nullptr) {
    SPDK_ERRLOG("bdev_rbd: failed to create dispatch ring size=%u for reactor=%p\n",
                rd->ring_size, reactor);
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

static void
teardown_reactor_dispatch(ReactorDispatch *rd)
{
  ReactorTeardownCtx ctx;
  ctx.rd = rd;

  if (spdk_get_thread() == rd->reactor) {
    reactor_teardown_dispatch(&ctx);
  } else {
    int rc = spdk_thread_send_msg(rd->reactor, reactor_teardown_dispatch, &ctx);
    if (rc != 0) {
      SPDK_ERRLOG("bdev_rbd: teardown send_msg failed rc=%d reactor=%p\n",
                  rc, rd->reactor);
      /* Best effort if the reactor is already gone. */
      if (rd->poller != nullptr) {
        spdk_poller_unregister(&rd->poller);
      }
      if (rd->work_ring != nullptr) {
        spdk_ring_free(rd->work_ring);
        rd->work_ring = nullptr;
      }
      ctx.done.store(true, std::memory_order_release);
    } else {
      while (!ctx.done.load(std::memory_order_acquire)) {
        spdk_delay_us(100);
      }
    }
  }

  delete rd;
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
  dispatch_to_reactor(static_cast<ReactorDispatch *>(m_dispatch),
                      m_reactor_thread, std::move(fn));
}

ContextWQ::Channel SpdkContextWQ::current_channel() const {
  struct spdk_thread *self = spdk_get_thread();
  if (self == nullptr) {
    return nullptr;
  }

  if (t_self_dispatch == nullptr || t_self_dispatch->reactor != self) {
    t_self_dispatch = get_reactor_dispatch(self);
  }
  return t_self_dispatch;
}

void SpdkContextWQ::post_channel(Channel channel, Work fn) {
  auto *rd = static_cast<ReactorDispatch *>(channel);
  if (rd == nullptr) {
    send_fn(std::move(fn));
    return;
  }

  dispatch_to_reactor(rd, rd->reactor, std::move(fn));
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

/*
 * Serial work is ordered per channel rather than globally. SPDK runs each
 * spdk_thread single-threaded, so two functors on one channel never overlap,
 * which is the guarantee AioCompletion::complete_external_callback() needs.
 * Running inline on any reactor keeps a channel-bound completion on the thread
 * that submitted the op, so bdev_rbd_io_complete() can complete the bdev_io
 * without hopping to the image's WQ reactor and back.
 *
 * Off-reactor callers (e.g. msgr workers) without a pinned channel still go
 * to m_reactor_thread. Prefer dispatch_serial_channel() when the submit
 * channel is known.
 *
 * post_serial() is deliberately left funnelling to m_reactor_thread: its
 * callers are the cold zero-pending path (when unpinned) and ContextWQ::queue()
 * off-channel, whose drain() accounting expects a single consumer.
 */
void SpdkContextWQ::dispatch_serial(Work fn) {
  if (spdk_get_thread() != nullptr) {
    fn();
    return;
  }

  send_fn(std::move(fn));
}

void SpdkContextWQ::dispatch_serial_channel(Channel channel, Work fn) {
  if (spdk_get_thread() != nullptr) {
    fn();
    return;
  }

  post_channel(channel, std::move(fn));
}

void SpdkContextWQ::drain() {
  /*
   * Wait until every tracked op finishes. Do not time out and return with work
   * still queued — the destructor would otherwise free this while callbacks
   * still capture `this`. Relies on the reactor continuing to poll.
   */
  const int log_every = 100000; /* ~10s at 100us */
  int iterations = 0;

  while (m_queued_ops.load(std::memory_order_acquire) > 0) {
    spdk_delay_us(100);
    ++iterations;
    if (iterations % log_every == 0) {
      SPDK_ERRLOG("SpdkContextWQ::drain: still waiting, queued_ops=%lu after %d iterations\n",
                  m_queued_ops.load(std::memory_order_acquire), iterations);
    }
  }
}

} // namespace asio
} // namespace librbd

// C API implementation
extern "C" {

int
bdev_rbd_wq_set_ring_size(uint32_t ring_size)
{
  if (ring_size < SPDK_WQ_RING_SIZE_MIN || ring_size > SPDK_WQ_RING_SIZE_MAX ||
      !is_power_of_two(ring_size)) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(g_dispatch_mutex);
  if (!g_dispatch_map.empty()) {
    return -EBUSY;
  }
  g_wq_ring_size = ring_size;
  SPDK_NOTICELOG("bdev_rbd: SpdkContextWQ ring_size set to %u\n", ring_size);
  return 0;
}

uint32_t
bdev_rbd_wq_get_ring_size(void)
{
  std::lock_guard<std::mutex> lk(g_dispatch_mutex);
  return g_wq_ring_size;
}

uint32_t
bdev_rbd_wq_get_poll_batch(void)
{
  return SPDK_WQ_POLL_BATCH;
}

bool
bdev_rbd_wq_rings_created(void)
{
  std::lock_guard<std::mutex> lk(g_dispatch_mutex);
  return !g_dispatch_map.empty();
}

void
bdev_rbd_wq_get_stats(bdev_rbd_wq_stats_fn fn, void *cb_arg)
{
  if (fn == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lk(g_dispatch_mutex);
  for (auto &kv : g_dispatch_map) {
    ReactorDispatch *rd = kv.second;
    struct bdev_rbd_wq_reactor_stats s = {};
    s.thread_id = spdk_thread_get_id(rd->reactor);
    const char *name = spdk_thread_get_name(rd->reactor);
    if (name != nullptr) {
      snprintf(s.thread_name, sizeof(s.thread_name), "%s", name);
    } else {
      s.thread_name[0] = '\0';
    }
    s.ring_size = rd->ring_size;
    s.fallback = rd->stats.fallback.load(std::memory_order_relaxed);
    s.deq_items = rd->stats.deq_items.load(std::memory_order_relaxed);
    s.deq_batches = rd->stats.deq_batches.load(std::memory_order_relaxed);
    s.deq_empty = rd->stats.deq_empty.load(std::memory_order_relaxed);
    s.depth = rd->work_ring != nullptr ? spdk_ring_count(rd->work_ring) : 0;
    s.depth_hwm = std::max(rd->stats.depth_hwm.load(std::memory_order_relaxed),
                           s.depth);
    fn(cb_arg, &s);
  }
}

void
bdev_rbd_wq_fini(void)
{
  std::vector<ReactorDispatch *> pending;

  {
    std::lock_guard<std::mutex> lk(g_dispatch_mutex);
    pending.reserve(g_dispatch_map.size());
    for (auto &kv : g_dispatch_map) {
      pending.push_back(kv.second);
    }
    g_dispatch_map.clear();
  }

  for (ReactorDispatch *rd : pending) {
    teardown_reactor_dispatch(rd);
  }

  SPDK_NOTICELOG("bdev_rbd: SpdkContextWQ dispatch contexts torn down (%zu)\n",
                 pending.size());
}

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
