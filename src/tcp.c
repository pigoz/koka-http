/*---------------------------------------------------------------------------
  TCP streams for Koka's `std/async`, on top of libuv.

  Included (inlined) by `tcp.kk` through `extern import c file "tcp.c"`.
  The helpers `kk_uv_loop`, `kk_uv_req_*`, `kk_result_*` and
  `kk_error_from_uv_errno` come from `std/async/api/uv/evloop`.

  Every libuv callback is turned into a _one-shot_ Koka callback so it fits
  the `await` primitive of `std/async`:
  - accept: resolves with one connection (a server can have many waiters),
  - read:   resolves with one chunk of bytes (empty bytes at end-of-stream),
  - write/shutdown: plain libuv requests.
  Reads, writes, and shutdowns take an optional timeout (a libuv timer): an
  expired read fails with `ETIMEDOUT`; an expired write or shutdown closes the
  connection (libuv cannot abort a write) and fails with `ETIMEDOUT`.

  Memory: a `kk_uvtcp_t` is shared between Koka (a raw C pointer box) and libuv
  (the handle). It is freed only after both the Koka box has been released
  _and_ the libuv close callback has run. Releasing the box of a handle that
  is still open closes it. While a callback is outstanding, the waiter holds a
  reference to the box so the handle stays alive.
---------------------------------------------------------------------------*/

#include <stdio.h>
#include <string.h>
#include <uv.h>
#if !defined(_WIN32)
#include <signal.h>
#endif

// Koka 3.2.9 links its own static libuv 1.52.1: the headers must match (see `vendor/libuv` and `mise.toml`).
#if UV_VERSION_MAJOR != 1 || UV_VERSION_MINOR != 52
#error "tcp.c: expected the libuv 1.52 headers from vendor/libuv/include (is KOKA_OPTIONS set? run through `mise exec`)"
#endif

// A one-shot timer for timeouts. It is closed (and freed) by `kk_uvtcp_timer_stop`,
// which the owner of the timer calls exactly once, also after the timer expired.
typedef struct kk_uvtcp_timer_s {
  uv_timer_t timer;             // must be the first field
  void     (*expired)(void* arg, kk_context_t* ctx);
  void*      arg;
} kk_uvtcp_timer_t;

struct kk_uvtcp_s;

// A Koka callback waiting on a handle (for accept or read).
typedef struct kk_uvtcp_waiter_s {
  struct kk_uvtcp_waiter_s* next;
  struct kk_uvtcp_waiter_s* prev;
  kk_function_t cb;             // `(error<a>) -> ioc ()`
  kk_box_t      self;           // keeps the tcp handle alive while waiting
  struct kk_uvtcp_s* t;         // the handle
  kk_uvtcp_timer_t*  timer;     // read timeout (or NULL)
} kk_uvtcp_waiter_t;

typedef struct kk_uvtcp_s {
  uv_tcp_t          tcp;        // must be the first field: we cast between `uv_handle_t*` and `kk_uvtcp_t*`
  kk_uvtcp_waiter_t*  first;      // queue of waiters (FIFO)
  kk_uvtcp_waiter_t*  last;
  bool              is_server;
  bool              closing;    // `uv_close` has been called
  bool              closed;     // the libuv close callback has run
  bool              released;   // the Koka box has been freed
  bool              reading;    // connection: `uv_read_start` is active
  bool              eof;        // connection: end-of-stream was reached
  int               err;        // server: pending listen error; connection: sticky read error
  int               pending;    // server: connections ready to be accepted
  bool              has_stash;  // connection: data that arrived while nobody was reading
  kk_bytes_t        stash;
} kk_uvtcp_t;


//---------------------------------------------------------------------------
// Timers
//---------------------------------------------------------------------------

static void kk_uvtcp_timer_close_cb(uv_handle_t* h) {
  kk_free(h, kk_get_context());
}

static void kk_uvtcp_timer_cb(uv_timer_t* h) {
  kk_uvtcp_timer_t* tm = (kk_uvtcp_timer_t*)h;
  tm->expired(tm->arg, kk_get_context());
}

// Start a timer that calls `expired(arg)` after `ms` milliseconds; with `ms <= 0` there is no timer (`*ptm == NULL`).
static int kk_uvtcp_timer_start(uv_loop_t* loop, int64_t ms, void (*expired)(void*, kk_context_t*), void* arg, kk_uvtcp_timer_t** ptm, kk_context_t* ctx) {
  *ptm = NULL;
  if (ms <= 0) return 0;
  kk_uvtcp_timer_t* tm = (kk_uvtcp_timer_t*)kk_zalloc(sizeof(kk_uvtcp_timer_t), ctx);
  if (tm == NULL) return UV_ENOMEM;
  int err = uv_timer_init(loop, &tm->timer);
  if (err != 0) {
    kk_free(tm, ctx);
    return err;
  }
  tm->expired = expired;
  tm->arg = arg;
  err = uv_timer_start(&tm->timer, &kk_uvtcp_timer_cb, (uint64_t)ms, 0);
  if (err != 0) {
    uv_close((uv_handle_t*)&tm->timer, &kk_uvtcp_timer_close_cb);
    return err;
  }
  *ptm = tm;
  return 0;
}

// Stop and release a timer (also fine after it expired).
static void kk_uvtcp_timer_stop(kk_uvtcp_timer_t* tm) {
  if (tm == NULL) return;
  uv_timer_stop(&tm->timer);
  uv_close((uv_handle_t*)&tm->timer, &kk_uvtcp_timer_close_cb);
}


//---------------------------------------------------------------------------
// Allocation, boxing, and closing
//---------------------------------------------------------------------------

static void kk_uvtcp_free_box(void* p, kk_block_t* block, kk_context_t* ctx);

static kk_uvtcp_t* kk_uvtcp_unbox(kk_box_t b, kk_context_t* ctx) {
  return (kk_uvtcp_t*)kk_cptr_raw_unbox_borrowed(b, ctx);
}

static kk_box_t kk_uvtcp_box(kk_uvtcp_t* t, kk_context_t* ctx) {
  return kk_cptr_raw_box(&kk_uvtcp_free_box, t, ctx);
}

static kk_uvtcp_t* kk_uvtcp_alloc(bool is_server, kk_context_t* ctx) {
  kk_uvtcp_t* t = (kk_uvtcp_t*)kk_zalloc(sizeof(kk_uvtcp_t), ctx);
  if (t != NULL) t->is_server = is_server;
  return t;
}

static void kk_uvtcp_mem_free(kk_uvtcp_t* t, kk_context_t* ctx) {
  if (t->has_stash) {
    t->has_stash = false;
    kk_bytes_drop(t->stash, ctx);
  }
  kk_free(t, ctx);
}

static void kk_uvtcp_waiter_unlink(kk_uvtcp_t* t, kk_uvtcp_waiter_t* w);

static void kk_uvtcp_close_cb(uv_handle_t* h) {
  kk_context_t* ctx = kk_get_context();
  kk_uvtcp_t* t = (kk_uvtcp_t*)h;
  t->closed = true;
  if (t->has_stash) {
    t->has_stash = false;
    kk_bytes_drop(t->stash, ctx);
  }
  if (t->first == NULL) {
    // no waiters hold a reference, so if the box is gone we are the last owner
    if (t->released) kk_uvtcp_mem_free(t, ctx);
    return;
  }
  // Resolve the waiters with `ECANCELED`, one at a time from the live list: a callback runs
  // Koka code that may cancel (dispose) a sibling waiter, which unlinks it from this list.
  // (No new waiters can be added since `closing` is set.)
  while (true) {
    kk_uvtcp_waiter_t* w = t->first;
    kk_uvtcp_waiter_unlink(t, w);
    kk_uvtcp_timer_stop(w->timer);
    kk_box_t self = w->self;
    kk_function_call_error(w->cb, kk_error_from_uv_errno(UV_ECANCELED, ctx), ctx);  // may dispose sibling waiters
    kk_free(w, ctx);
    // `t` is still alive (we hold `self`); after we drop it, the remaining waiters keep it alive
    const bool more = (t->first != NULL);
    kk_box_drop(self, ctx);   // if `!more`, this may free `t`
    if (!more) break;
  }
}

// Close the handle (idempotent). The memory is freed later (see `kk_uvtcp_close_cb`).
static void kk_uvtcp_close(kk_uvtcp_t* t) {
  if (t->closing) return;
  t->closing = true;
  t->reading = false;
  uv_close((uv_handle_t*)&t->tcp, &kk_uvtcp_close_cb);  // stops reading/listening; pending writes get `ECANCELED`
}

// Called when the Koka box is freed.
static void kk_uvtcp_free_box(void* p, kk_block_t* block, kk_context_t* ctx) {
  kk_unused(block);
  kk_uvtcp_t* t = (kk_uvtcp_t*)p;
  if (t == NULL) return;
  t->released = true;
  if (!t->closing) {
    kk_uvtcp_close(t);            // the close callback frees the memory
  }
  else if (t->closed) {
    kk_uvtcp_mem_free(t, ctx);    // the close callback already ran
  }
}

// Close a handle from Koka (borrowed).
kk_unit_t kk_uvtcp_close_handle(kk_box_t h, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(h, ctx);
  if (t != NULL) kk_uvtcp_close(t);
  return kk_Unit;
}


//---------------------------------------------------------------------------
// Waiters
//---------------------------------------------------------------------------

static kk_uvtcp_waiter_t* kk_uvtcp_waiter_push(kk_uvtcp_t* t, kk_function_t cb, kk_box_t self, kk_context_t* ctx) {
  kk_uvtcp_waiter_t* w = (kk_uvtcp_waiter_t*)kk_zalloc(sizeof(kk_uvtcp_waiter_t), ctx);
  if (w == NULL) return NULL;
  w->cb = cb;
  w->self = self;
  w->t = t;
  w->prev = t->last;
  if (t->last != NULL) t->last->next = w; else t->first = w;
  t->last = w;
  return w;
}

static void kk_uvtcp_waiter_unlink(kk_uvtcp_t* t, kk_uvtcp_waiter_t* w) {
  if (w->prev != NULL) w->prev->next = w->next; else t->first = w->next;
  if (w->next != NULL) w->next->prev = w->prev; else t->last = w->prev;
  w->next = w->prev = NULL;
}

static void kk_uvtcp_read_stop(kk_uvtcp_t* t) {
  if (t->reading && !t->closing) {
    uv_read_stop((uv_stream_t*)&t->tcp);
  }
  t->reading = false;
}

// Resolve (and free) an unlinked waiter. `t` may be freed after this returns.
static void kk_uvtcp_waiter_resolve(kk_uvtcp_waiter_t* w, kk_std_core_exn__error res, kk_context_t* ctx) {
  kk_uvtcp_timer_stop(w->timer);
  kk_box_t self = w->self;
  kk_function_call_error(w->cb, res, ctx);   // drops `w->cb`
  kk_free(w, ctx);
  kk_box_drop(self, ctx);
}

// Dispose function: called by `std/async` when an outstanding accept/read is canceled.
static void kk_uvtcp_waiter_dispose(uv_handle_t* h, void* arg, kk_context_t* ctx) {
  kk_uvtcp_t* t = (kk_uvtcp_t*)h;
  kk_uvtcp_waiter_t* w = (kk_uvtcp_waiter_t*)arg;
  kk_uvtcp_waiter_unlink(t, w);
  if (!t->is_server && t->first == NULL) kk_uvtcp_read_stop(t);  // apply backpressure again
  kk_uvtcp_timer_stop(w->timer);
  kk_box_t self = w->self;
  kk_function_drop(w->cb, ctx);
  kk_free(w, ctx);
  kk_box_drop(self, ctx);   // last: may free `t`
}

static void kk_uvtcp_noop_dispose(uv_handle_t* h, void* arg, kk_context_t* ctx) {
  kk_unused(h); kk_unused(arg); kk_unused(ctx);
}


//---------------------------------------------------------------------------
// Listen and accept
//---------------------------------------------------------------------------

// Accept one pending connection (or report the pending listen error).
static kk_std_core_exn__error kk_uvtcp_accept_one(kk_uvtcp_t* srv, kk_context_t* ctx) {
  if (srv->pending <= 0) {
    int err = (srv->err != 0 ? srv->err : UV_EAGAIN);
    srv->err = 0;
    return kk_error_from_uv_errno(err, ctx);
  }
  srv->pending--;
  kk_uvtcp_t* c = kk_uvtcp_alloc(false, ctx);
  if (c == NULL) return kk_error_from_uv_errno(UV_ENOMEM, ctx);
  int err = uv_tcp_init(srv->tcp.loop, &c->tcp);
  if (err != 0) {
    kk_free(c, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  err = uv_accept((uv_stream_t*)&srv->tcp, (uv_stream_t*)&c->tcp);
  if (err != 0) {
    if (err == UV_EAGAIN) srv->pending = 0;
    c->released = true;
    kk_uvtcp_close(c);
    return kk_error_from_uv_errno(err, ctx);
  }
  uv_tcp_nodelay(&c->tcp, 1);
  return kk_result_ok(kk_uvtcp_box(c, ctx), ctx);
}

// Hand out pending connections (or errors) to waiting accepts.
static void kk_uvtcp_accept_resolve_waiters(kk_uvtcp_t* srv, kk_context_t* ctx) {
  while (!srv->closing && srv->first != NULL && (srv->pending > 0 || srv->err != 0)) {
    kk_uvtcp_waiter_t* w = srv->first;
    kk_uvtcp_waiter_unlink(srv, w);
    kk_std_core_exn__error res = kk_uvtcp_accept_one(srv, ctx);
    // note: `srv` stays valid: it is only freed after its close callback ran, which is never synchronous
    kk_uvtcp_waiter_resolve(w, res, ctx);
  }
}

static void kk_uvtcp_connection_cb(uv_stream_t* server, int status) {
  kk_context_t* ctx = kk_get_context();
  kk_uvtcp_t* srv = (kk_uvtcp_t*)server;
  if (status < 0) srv->err = status;
             else srv->pending++;
  // Note: on unix, libuv stops accepting until we call `uv_accept`; connections that
  // arrive while no one is accepting wait in the kernel backlog (backpressure).
  kk_uvtcp_accept_resolve_waiters(srv, ctx);
}

// Parse `host` (IPv4 or IPv6 literal) and `port` into `addr`.
static int kk_uvtcp_parse_addr(kk_string_t host, int32_t port, struct sockaddr_storage* addr, kk_context_t* ctx) {
  int err = UV_EINVAL;
  memset(addr, 0, sizeof(*addr));
  kk_with_string_as_qutf8_borrow(host, chost, ctx) {
    if (strchr(chost, ':') != NULL) err = uv_ip6_addr(chost, port, (struct sockaddr_in6*)addr);
                               else err = uv_ip4_addr(chost, port, (struct sockaddr_in*)addr);
  }
  return err;
}

// Writing to a connection that the peer reset raises SIGPIPE on unix, which would kill the process.
static void kk_uvtcp_ignore_sigpipe(void) {
  #if !defined(_WIN32)
  static bool ignored = false;
  if (!ignored) {
    ignored = true;
    signal(SIGPIPE, SIG_IGN);
  }
  #endif
}

// Start listening on `host:port`; `loop` and `host` are borrowed.
kk_std_core_exn__error kk_uvtcp_listen(kk_box_t loop, kk_string_t host, int32_t port, int32_t backlog, kk_context_t* ctx) {
  kk_uvtcp_ignore_sigpipe();
  if (port < 0 || port > 65535) return kk_error_from_uv_errno(UV_EINVAL, ctx);
  struct sockaddr_storage addr;
  int err = kk_uvtcp_parse_addr(host, port, &addr, ctx);
  if (err != 0) return kk_error_from_uv_errno(err, ctx);
  kk_uvtcp_t* t = kk_uvtcp_alloc(true, ctx);
  if (t == NULL) return kk_error_from_uv_errno(UV_ENOMEM, ctx);
  err = uv_tcp_init(kk_uv_loop(loop, ctx), &t->tcp);
  if (err != 0) {
    kk_free(t, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  err = uv_tcp_bind(&t->tcp, (const struct sockaddr*)&addr, 0);
  if (err == 0) err = uv_listen((uv_stream_t*)&t->tcp, backlog, &kk_uvtcp_connection_cb);
  if (err != 0) {
    t->released = true;
    kk_uvtcp_close(t);
    return kk_error_from_uv_errno(err, ctx);
  }
  return kk_result_ok(kk_uvtcp_box(t, ctx), ctx);
}

// Await a connection: `srv` and `cb` are owned.
kk_std_core_exn__error kk_uvtcp_accept_setup(kk_box_t srv, kk_function_t cb, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(srv, ctx);
  if (t->closing) {
    kk_function_drop(cb, ctx);
    kk_box_drop(srv, ctx);
    return kk_error_from_uv_errno(UV_EBADF, ctx);
  }
  kk_uvtcp_waiter_t* w = kk_uvtcp_waiter_push(t, cb, srv, ctx);
  if (w == NULL) {
    kk_function_drop(cb, ctx);
    kk_box_drop(srv, ctx);
    return kk_error_from_uv_errno(UV_ENOMEM, ctx);
  }
  if (t->first == w && (t->pending > 0 || t->err != 0)) {
    // a connection is ready: resolve right away (`std/async` supports resuming during setup)
    kk_uvtcp_waiter_unlink(t, w);
    kk_std_core_exn__error res = kk_uvtcp_accept_one(t, ctx);
    kk_uvtcp_waiter_resolve(w, res, ctx);
    return kk_result_uv_handle_dispose((uv_handle_t*)t, NULL, &kk_uvtcp_noop_dispose, ctx);
  }
  return kk_result_uv_handle_dispose((uv_handle_t*)t, w, &kk_uvtcp_waiter_dispose, ctx);
}


//---------------------------------------------------------------------------
// Reading
//---------------------------------------------------------------------------

// On unix, libuv calls `read_cb` right after `alloc_cb`, so one static buffer suffices;
// we fall back to a fresh allocation if it happens to be in use.
static char kk_uvtcp_rbuf[65536];
static bool kk_uvtcp_rbuf_in_use = false;

static void kk_uvtcp_alloc_cb(uv_handle_t* h, size_t suggested, uv_buf_t* buf) {
  kk_unused(h);
  if (!kk_uvtcp_rbuf_in_use) {
    kk_uvtcp_rbuf_in_use = true;
    *buf = uv_buf_init(kk_uvtcp_rbuf, sizeof(kk_uvtcp_rbuf));
  }
  else {
    char* p = (char*)kk_malloc((kk_ssize_t)suggested, kk_get_context());
    *buf = uv_buf_init(p, (p == NULL ? 0 : (unsigned int)suggested));  // a zero length makes libuv report `ENOBUFS`
  }
}

static void kk_uvtcp_buf_release(const uv_buf_t* buf) {
  if (buf->base == kk_uvtcp_rbuf) kk_uvtcp_rbuf_in_use = false;
  else if (buf->base != NULL) kk_free(buf->base, kk_get_context());
}

static bool kk_uvtcp_read_available(kk_uvtcp_t* t) {
  return (t->has_stash || t->eof || t->err != 0);
}

// Take the next read result: data first, then the (sticky) error or end-of-stream.
static kk_std_core_exn__error kk_uvtcp_read_take(kk_uvtcp_t* t, kk_context_t* ctx) {
  if (t->has_stash) {
    t->has_stash = false;
    return kk_result_ok(kk_bytes_box(t->stash), ctx);
  }
  if (t->err != 0) return kk_error_from_uv_errno(t->err, ctx);
  return kk_result_ok(kk_bytes_box(kk_bytes_empty()), ctx);  // end-of-stream
}

// Resolve the waiting reader (if any) with the available data.
static void kk_uvtcp_read_resolve(kk_uvtcp_t* t, kk_context_t* ctx) {
  kk_uvtcp_waiter_t* w = t->first;
  if (w == NULL || !kk_uvtcp_read_available(t)) return;
  kk_uvtcp_waiter_unlink(t, w);
  kk_std_core_exn__error res = kk_uvtcp_read_take(t, ctx);
  kk_uvtcp_read_stop(t);             // stop until the next read (backpressure)
  kk_uvtcp_waiter_resolve(w, res, ctx);  // `t` stays valid (freed only after the close callback)
}

static void kk_uvtcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  kk_context_t* ctx = kk_get_context();
  kk_uvtcp_t* t = (kk_uvtcp_t*)stream;
  if (nread > 0) {
    kk_bytes_t data = kk_bytes_alloc_dupn((kk_ssize_t)nread, (const uint8_t*)buf->base, ctx);
    if (t->has_stash) {
      t->stash = kk_bytes_cat(t->stash, data, ctx);
    }
    else {
      t->stash = data;
      t->has_stash = true;
    }
  }
  else if (nread == UV_EOF) {
    t->eof = true;
  }
  else if (nread < 0) {
    t->err = (int)nread;
  }
  kk_uvtcp_buf_release(buf);
  if (nread == 0) return;          // `EAGAIN`: nothing read
  if (t->first == NULL) {
    kk_uvtcp_read_stop(t);           // nobody is reading: keep the data and stop
  }
  else {
    kk_uvtcp_read_resolve(t, ctx);
  }
}

// A read timed out: fail it with `ETIMEDOUT`.
static void kk_uvtcp_read_expired(void* arg, kk_context_t* ctx) {
  kk_uvtcp_waiter_t* w = (kk_uvtcp_waiter_t*)arg;
  kk_uvtcp_t* t = w->t;
  kk_uvtcp_waiter_unlink(t, w);
  if (t->first == NULL) kk_uvtcp_read_stop(t);
  kk_uvtcp_waiter_resolve(w, kk_error_from_uv_errno(UV_ETIMEDOUT, ctx), ctx);  // also releases the timer
}

// Await the next chunk of data (failing with `ETIMEDOUT` after `timeout_ms` if positive): `conn` and `cb` are owned.
kk_std_core_exn__error kk_uvtcp_read_setup(kk_box_t conn, int64_t timeout_ms, kk_function_t cb, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(conn, ctx);
  int err = 0;
  if (t->closing) err = UV_EBADF;
  else if (t->first != NULL) err = UV_EALREADY;  // only one reader at a time
  if (err != 0) {
    kk_function_drop(cb, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  kk_uvtcp_waiter_t* w = kk_uvtcp_waiter_push(t, cb, conn, ctx);
  if (w == NULL) {
    kk_function_drop(cb, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(UV_ENOMEM, ctx);
  }
  if (kk_uvtcp_read_available(t)) {
    kk_uvtcp_read_resolve(t, ctx);   // resolve right away
    return kk_result_uv_handle_dispose((uv_handle_t*)t, NULL, &kk_uvtcp_noop_dispose, ctx);
  }
  err = kk_uvtcp_timer_start(t->tcp.loop, timeout_ms, &kk_uvtcp_read_expired, w, &w->timer, ctx);
  if (err == 0 && !t->reading) {
    err = uv_read_start((uv_stream_t*)&t->tcp, &kk_uvtcp_alloc_cb, &kk_uvtcp_read_cb);
    if (err == 0) t->reading = true;
  }
  if (err != 0) {
    kk_uvtcp_waiter_unlink(t, w);
    kk_uvtcp_timer_stop(w->timer);
    kk_function_drop(w->cb, ctx);
    kk_free(w, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  return kk_result_uv_handle_dispose((uv_handle_t*)t, w, &kk_uvtcp_waiter_dispose, ctx);
}


//---------------------------------------------------------------------------
// Writing and shutdown
//---------------------------------------------------------------------------

typedef struct kk_uvtcp_write_s {
  uv_write_t req;         // must be the first field
  kk_bytes_t buf;         // kept alive until the write completes
  kk_box_t   conn;        // keeps the connection alive until the write completes
  int        status;
  kk_uvtcp_timer_t* timer;
  bool       timed_out;
} kk_uvtcp_write_t;

// A write or shutdown timed out: libuv cannot abort it, so we close the connection,
// which completes the request with `ECANCELED` (reported as `ETIMEDOUT`).
static void kk_uvtcp_conn_expired(kk_box_t conn, bool* timed_out, kk_context_t* ctx) {
  *timed_out = true;
  kk_uvtcp_close(kk_uvtcp_unbox(conn, ctx));
}

static void kk_uvtcp_write_expired(void* arg, kk_context_t* ctx) {
  kk_uvtcp_write_t* w = (kk_uvtcp_write_t*)arg;
  kk_uvtcp_conn_expired(w->conn, &w->timed_out, ctx);
}

static void kk_uvtcp_unit_result_call(kk_function_t cb, int status, kk_context_t* ctx) {
  kk_std_core_exn__error res = (status < 0 ? kk_error_from_uv_errno(status, ctx)
                                           : kk_result_ok(kk_unit_box(kk_Unit), ctx));
  kk_function_call_error(cb, res, ctx);
}

static void kk_uvtcp_write_call(kk_function_t cb, uv_req_t* req, kk_context_t* ctx) {
  kk_uvtcp_unit_result_call(cb, ((kk_uvtcp_write_t*)req)->status, ctx);
}

static void kk_uvtcp_write_cb(uv_write_t* req, int status) {
  kk_context_t* ctx = kk_get_context();
  kk_uvtcp_write_t* w = (kk_uvtcp_write_t*)req;
  kk_bytes_t buf = w->buf;
  kk_box_t conn  = w->conn;
  kk_uvtcp_timer_stop(w->timer);
  w->timer = NULL;
  w->status = (w->timed_out ? UV_ETIMEDOUT : status);
  kk_uv_req_callback((uv_req_t*)req, &kk_uvtcp_write_call);  // calls the Koka callback (unless canceled) and frees `req`
  kk_bytes_drop(buf, ctx);
  kk_box_drop(conn, ctx);
}

// Write all of `buf` (closing the connection if that takes more than `timeout_ms`, if positive):
// `conn`, `buf`, and `cb` are owned. Canceling a write does not abort it (libuv cannot),
// but the callback is no longer called.
kk_std_core_exn__error kk_uvtcp_write_setup(kk_box_t conn, kk_bytes_t buf, int64_t timeout_ms, kk_function_t cb, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(conn, ctx);
  if (t->closing) {
    kk_function_drop(cb, ctx);
    kk_bytes_drop(buf, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(UV_EBADF, ctx);
  }
  kk_uvtcp_write_t* w;
  int err = kk_uv_req_create(sizeof(kk_uvtcp_write_t), cb, (uv_req_t**)&w, ctx);  // takes `cb`
  if (err != 0) {
    kk_bytes_drop(buf, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  kk_ssize_t len = 0;
  const uint8_t* base = kk_bytes_buf_borrow(buf, &len, ctx);
  uv_buf_t ubuf = uv_buf_init((char*)base, (unsigned int)len);
  w->buf  = buf;
  w->conn = conn;
  err = uv_write(&w->req, (uv_stream_t*)&t->tcp, &ubuf, 1, &kk_uvtcp_write_cb);
  if (err == 0) {
    err = kk_uvtcp_timer_start(t->tcp.loop, timeout_ms, &kk_uvtcp_write_expired, w, &w->timer, ctx);
    if (err != 0) {
      // the write is under way: close the connection (the write callback frees the request)
      w->timed_out = false;
      kk_uvtcp_close(t);
      return kk_result_uv_req_dispose0((uv_req_t*)w, ctx);
    }
  }
  if (err != 0) {
    kk_uv_req_free((uv_req_t*)w, ctx);  // drops `cb`
    kk_bytes_drop(buf, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  return kk_result_uv_req_dispose0((uv_req_t*)w, ctx);
}

typedef struct kk_uvtcp_shutdown_s {
  uv_shutdown_t req;      // must be the first field
  kk_box_t      conn;
  int           status;
  kk_uvtcp_timer_t* timer;
  bool          timed_out;
} kk_uvtcp_shutdown_t;

static void kk_uvtcp_shutdown_expired(void* arg, kk_context_t* ctx) {
  kk_uvtcp_shutdown_t* s = (kk_uvtcp_shutdown_t*)arg;
  kk_uvtcp_conn_expired(s->conn, &s->timed_out, ctx);
}

static void kk_uvtcp_shutdown_call(kk_function_t cb, uv_req_t* req, kk_context_t* ctx) {
  kk_uvtcp_unit_result_call(cb, ((kk_uvtcp_shutdown_t*)req)->status, ctx);
}

static void kk_uvtcp_shutdown_cb(uv_shutdown_t* req, int status) {
  kk_context_t* ctx = kk_get_context();
  kk_uvtcp_shutdown_t* s = (kk_uvtcp_shutdown_t*)req;
  kk_box_t conn = s->conn;
  kk_uvtcp_timer_stop(s->timer);
  s->timer = NULL;
  s->status = (s->timed_out ? UV_ETIMEDOUT : status);
  kk_uv_req_callback((uv_req_t*)req, &kk_uvtcp_shutdown_call);
  kk_box_drop(conn, ctx);
}

// Shut down the write side after all pending writes are done (closing the connection if that
// takes more than `timeout_ms`, if positive): `conn` and `cb` are owned.
kk_std_core_exn__error kk_uvtcp_shutdown_setup(kk_box_t conn, int64_t timeout_ms, kk_function_t cb, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(conn, ctx);
  if (t->closing) {
    kk_function_drop(cb, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(UV_EBADF, ctx);
  }
  kk_uvtcp_shutdown_t* s;
  int err = kk_uv_req_create(sizeof(kk_uvtcp_shutdown_t), cb, (uv_req_t**)&s, ctx);
  if (err != 0) {
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  s->conn = conn;
  err = uv_shutdown(&s->req, (uv_stream_t*)&t->tcp, &kk_uvtcp_shutdown_cb);
  if (err == 0) {
    err = kk_uvtcp_timer_start(t->tcp.loop, timeout_ms, &kk_uvtcp_shutdown_expired, s, &s->timer, ctx);
    if (err != 0) {
      kk_uvtcp_close(t);   // the shutdown callback frees the request
      return kk_result_uv_req_dispose0((uv_req_t*)s, ctx);
    }
  }
  if (err != 0) {
    kk_uv_req_free((uv_req_t*)s, ctx);
    kk_box_drop(conn, ctx);
    return kk_error_from_uv_errno(err, ctx);
  }
  return kk_result_uv_req_dispose0((uv_req_t*)s, ctx);
}


//---------------------------------------------------------------------------
// Addresses
//---------------------------------------------------------------------------

static kk_string_t kk_uvtcp_addr_string(const struct sockaddr_storage* addr, kk_context_t* ctx) {
  char ip[INET6_ADDRSTRLEN + 1] = { 0 };
  char out[INET6_ADDRSTRLEN + 16] = { 0 };
  if (addr->ss_family == AF_INET6) {
    const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)addr;
    uv_ip6_name(a6, ip, sizeof(ip));
    snprintf(out, sizeof(out), "[%s]:%d", ip, ntohs(a6->sin6_port));
  }
  else if (addr->ss_family == AF_INET) {
    const struct sockaddr_in* a4 = (const struct sockaddr_in*)addr;
    uv_ip4_name(a4, ip, sizeof(ip));
    snprintf(out, sizeof(out), "%s:%d", ip, ntohs(a4->sin_port));
  }
  return kk_string_alloc_from_qutf8(out, ctx);
}

// The remote address of a connection (borrowed).
kk_string_t kk_uvtcp_peer_name(kk_box_t conn, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(conn, ctx);
  struct sockaddr_storage addr;
  int len = sizeof(addr);
  memset(&addr, 0, sizeof(addr));
  if (t->closing || uv_tcp_getpeername(&t->tcp, (struct sockaddr*)&addr, &len) != 0) {
    return kk_string_alloc_from_qutf8("", ctx);
  }
  return kk_uvtcp_addr_string(&addr, ctx);
}

// The local address of a handle (borrowed); useful to find the port after listening on port 0.
kk_string_t kk_uvtcp_sock_name(kk_box_t h, kk_context_t* ctx) {
  kk_uvtcp_t* t = kk_uvtcp_unbox(h, ctx);
  struct sockaddr_storage addr;
  int len = sizeof(addr);
  memset(&addr, 0, sizeof(addr));
  if (t->closing || uv_tcp_getsockname(&t->tcp, (struct sockaddr*)&addr, &len) != 0) {
    return kk_string_alloc_from_qutf8("", ctx);
  }
  return kk_uvtcp_addr_string(&addr, ctx);
}


//---------------------------------------------------------------------------
// Misc
//---------------------------------------------------------------------------

// Is the handle closed (or closing)? (borrowed)
bool kk_uvtcp_is_closing(kk_box_t h, kk_context_t* ctx) {
  return kk_uvtcp_unbox(h, ctx)->closing;
}

// A monotonic clock in milliseconds.
int64_t kk_uvtcp_now_ms(kk_context_t* ctx) {
  kk_unused(ctx);
  return (int64_t)(uv_hrtime() / 1000000);
}

// The `errno` of a timed out operation (as in `ExnSystem(errno)`).
int32_t kk_uvtcp_etimedout(kk_context_t* ctx) {
  kk_unused(ctx);
  return -UV_ETIMEDOUT;
}
