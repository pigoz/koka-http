/*---------------------------------------------------------------------------
  TCP streams for `std/async`, using libuv through the C bindings in `tcp.c`.

  All operations are asynchronous and cancelable. Reads, writes, and shutdowns
  also take a `timeout` (implemented with a libuv timer, see `tcp.c`): prefer it
  over wrapping them in `timeout`/`with-timeout`, which is more costly.
---------------------------------------------------------------------------*/
module tcp

import std/num/int32
import std/num/int64
import std/time/duration
import std/async
import std/async/internal       // `async-ioc`
import std/async/api/uv/evloop  // C helpers used by `tcp.c` (`kk_uv_loop`, `kk_uv_req_*`, ...)

extern import
  c file "tcp.c"

// A listening TCP socket.
abstract struct tcp-server( uvh : any )

// A TCP connection.
abstract struct tcp-conn( uvh : any )


// ----------------------------------------------------------------------------
// Primitives (see `tcp.c`)
// ----------------------------------------------------------------------------

extern prim-listen( ^evloop : any, ^host : string, port : int32, backlog : int32 ) : ioc error<any>
  c "kk_uvtcp_listen"

extern prim-accept-setup( srv : any, cb : error<any> -> ioc () ) : ioc error<() -> ioc ()>
  c "kk_uvtcp_accept_setup"

extern prim-read-setup( conn : any, timeout-ms : int64, cb : error<bytes> -> ioc () ) : ioc error<() -> ioc ()>
  c "kk_uvtcp_read_setup"

extern prim-write-setup( conn : any, buf : bytes, timeout-ms : int64, cb : error<()> -> ioc () ) : ioc error<() -> ioc ()>
  c "kk_uvtcp_write_setup"

extern prim-shutdown-setup( conn : any, timeout-ms : int64, cb : error<()> -> ioc () ) : ioc error<() -> ioc ()>
  c "kk_uvtcp_shutdown_setup"

extern prim-close( ^h : any ) : ioc ()
  c "kk_uvtcp_close_handle"

extern prim-peer-name( ^conn : any ) : ioc string
  c "kk_uvtcp_peer_name"

extern prim-sock-name( ^h : any ) : ioc string
  c "kk_uvtcp_sock_name"

extern prim-is-closing( ^h : any ) : ioc bool
  c "kk_uvtcp_is_closing"

extern prim-now-ms() : ndet int64
  c "kk_uvtcp_now_ms"

extern prim-etimedout() : int32
  c "kk_uvtcp_etimedout"


// ----------------------------------------------------------------------------
// Server
// ----------------------------------------------------------------------------

// Listen for connections on `host` (an IPv4 or IPv6 address literal) and `port`.
// Use port 0 to let the OS pick a free port (see `server/local-address`).
pub fun listen( host : string, port : int, backlog : int = 511 ) : <async,exn> tcp-server
  with try-exn-prepend("listen on " ++ host ++ ":" ++ port.show)
  if port < 0 || port > 65535 then throw("invalid port")
  val h = await("tcp-listen") fn(evloop,resume)
    match prim-listen(evloop, host, port.int32, backlog.int32)
      Ok(srv)    ->
        resume(Ok(srv))   // completes right away
        Ok(fn() ())
      Error(exn) -> Error(exn)
  Tcp-server(h)

// Wait for the next incoming connection.
pub fun accept( srv : tcp-server ) : <async,exn> tcp-conn
  Tcp-conn(await("tcp-accept", fn(_evloop,resume) prim-accept-setup(srv.uvh, resume)))

// The local address of the server (as `ip:port`).
pub fun server/local-address( srv : tcp-server ) : async string
  async-ioc{ prim-sock-name(srv.uvh) }

// Stop listening. Waiting `accept`s fail with an `ECANCELED` error.
pub fun server/close( srv : tcp-server ) : async ()
  async-ioc{ prim-close(srv.uvh) }

// Has the server been closed?
pub fun server/is-closed( srv : tcp-server ) : async bool
  async-ioc{ prim-is-closing(srv.uvh) }


// ----------------------------------------------------------------------------
// Connections
// ----------------------------------------------------------------------------

// Read the next chunk of bytes (at most 64 KiB). Returns empty bytes at end-of-stream.
// With a positive `timeout`, fails with a timeout error (see `is-timeout`) if no data arrives in time.
pub fun read( conn : tcp-conn, timeout : duration = zero ) : <async,exn> bytes
  await("tcp-read", fn(_evloop,resume) prim-read-setup(conn.uvh, timeout.ms, resume))

// Write all bytes of `buf`. With a positive `timeout`, the connection is closed and a
// timeout error raised if the write does not complete in time (e.g. the peer stopped reading).
pub fun write( conn : tcp-conn, buf : bytes, timeout : duration = zero ) : <async,exn> ()
  if buf.is-empty then () else
    await("tcp-write", fn(_evloop,resume) prim-write-setup(conn.uvh, buf, timeout.ms, resume))

// Write a string (as UTF-8).
pub fun write-text( conn : tcp-conn, s : string, timeout : duration = zero ) : <async,exn> ()
  conn.write(s.bytes, timeout)

// Shut down the write side of the connection once all pending writes are done
// (sends a FIN to the peer); reading is still possible. `timeout` works as for `write`.
pub fun shutdown( conn : tcp-conn, timeout : duration = zero ) : <async,exn> ()
  await("tcp-shutdown", fn(_evloop,resume) prim-shutdown-setup(conn.uvh, timeout.ms, resume))

// Close the connection. Pending reads fail with `ECANCELED`.
pub fun conn/close( conn : tcp-conn ) : async ()
  async-ioc{ prim-close(conn.uvh) }

// The address of the peer (as `ip:port`).
pub fun remote-address( conn : tcp-conn ) : async string
  async-ioc{ prim-peer-name(conn.uvh) }

// The local address of the connection (as `ip:port`).
pub fun conn/local-address( conn : tcp-conn ) : async string
  async-ioc{ prim-sock-name(conn.uvh) }


// ----------------------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------------------

// Did an operation fail because its `timeout` expired?
pub fun is-timeout( exn : exception ) : bool
  match exn.info
    ExnSystem(errno) -> errno == prim-etimedout().int
    _                -> False

// A monotonic clock in milliseconds (for deadlines).
pub fun monotonic-ms() : ndet int
  prim-now-ms().int

fun duration/ms( d : duration ) : int64
  max(0, d.milli-seconds).int64
