/*---------------------------------------------------------------------------
  A minimal HTTP/1.1 server on top of `tcp`, `nursery`, and `httpparse` (llhttp).

  Supports keep-alive, pipelining, `Content-Length` and chunked request bodies,
  and `Expect: 100-continue`. Request framing is checked strictly by llhttp, and
  request targets by `origin-target`.
---------------------------------------------------------------------------*/
module http

import std/time/duration
import std/async
import tcp
import nursery
import httpparse

// An HTTP request.
pub struct request
  method  : string                  // e.g. "GET"
  target  : string                  // path and query (origin-form), e.g. "/search?q=koka"
  version : string                  // "HTTP/1.1" or "HTTP/1.0"
  headers : list<(string,string)>   // header names are lower case
  body    : bytes
  remote  : string                  // address of the client

// An HTTP response.
pub struct response
  status  : int
  headers : list<(string,string)> = []
  body    : bytes = empty

// Server limits and timeouts.
pub struct config
  max-head      : int      = 16 * 1024          // bytes of request line + headers
  max-body      : int      = 1024 * 1024        // bytes of a request body
  head-timeout  : duration = 10.seconds         // to receive a complete request head (also the keep-alive idle time)
  body-timeout  : duration = 30.seconds         // to receive a request body
  write-timeout : duration = 30.seconds         // to send a response


// ----------------------------------------------------------------------------
// Requests and responses
// ----------------------------------------------------------------------------

// The value of a request header (case insensitive).
pub fun request/header( req : request, name : string ) : maybe<string>
  val lname = name.to-lower
  req.headers.lookup(fn(k) k == lname)

// The path of the request target (without the query).
pub fun request/path( req : request ) : string
  match req.target.split("?", 2)
    Cons(path, _) -> path
    Nil           -> req.target

// The query of the request target (after the `?`), if any.
pub fun request/query( req : request ) : string
  match req.target.split("?", 2)
    Cons(_, Cons(query, _)) -> query
    _                       -> ""

// The request body as a string.
pub fun request/text( req : request ) : string
  req.body.string

// The path split into percent-decoded segments: `/hello/Ada%20L` gives `["hello", "Ada L"]`,
// and `/` gives `[]`. Handy for routing with `match`. Empty segments are kept (`/a//b/` gives
// `["a", "", "b", ""]`), and the server already rejected requests with `.`/`..` segments or with
// an encoded `/`, `\`, or NUL, so a segment is never a path by itself.
pub fun request/segments( req : request ) : list<string>
  match req.path.split("/")
    Cons("", Cons("", Nil)) -> []                          // "/"
    Cons("", segs)          -> segs.map(percent-decode)
    segs                    -> segs.map(percent-decode)    // "*" (for `OPTIONS *`)

// Decode `%XX` escapes in a URL component (invalid escapes are kept as is).
pub fun percent-decode( s : string ) : string
  fun decode( xs : list<int> ) : list<int>
    match xs
      Cons(37, Cons(h, Cons(l, rest))) | hex-value(h) >= 0 && hex-value(l) >= 0   // '%' h l
        -> Cons(hex-value(h) * 16 + hex-value(l), decode(rest))
      Cons(x, rest) -> Cons(x, decode(rest))
      Nil -> Nil
  if s.find("%").is-nothing then s else decode(s.bytes.list).vector.bytes.string

// The value of an ASCII hex digit, or -1.
fun hex-value( b : int ) : int
  if b >= 48 && b <= 57 then b - 48         // 0-9
  elif b >= 97 && b <= 102 then b - 87      // a-f
  elif b >= 65 && b <= 70 then b - 55       // A-F
  else -1

// A plain text response.
pub fun text( body : string, status : int = 200 ) : response
  Response(status, [("Content-Type", "text/plain; charset=utf-8")], body.bytes)

// An HTML response.
pub fun html( body : string, status : int = 200 ) : response
  Response(status, [("Content-Type", "text/html; charset=utf-8")], body.bytes)

// The standard reason phrase of a status code.
pub fun reason( status : int ) : string
  match status
    100 -> "Continue"
    200 -> "OK"
    201 -> "Created"
    204 -> "No Content"
    301 -> "Moved Permanently"
    302 -> "Found"
    304 -> "Not Modified"
    400 -> "Bad Request"
    404 -> "Not Found"
    405 -> "Method Not Allowed"
    408 -> "Request Timeout"
    413 -> "Content Too Large"
    431 -> "Request Header Fields Too Large"
    500 -> "Internal Server Error"
    501 -> "Not Implemented"
    503 -> "Service Unavailable"
    505 -> "HTTP Version Not Supported"
    _   -> "Unknown"


// ----------------------------------------------------------------------------
// Errors
// ----------------------------------------------------------------------------

extend type exception-info
  // A request that must be answered with an error `status` (after which the connection is closed).
  HttpError( status : int )

fun http-error( status : int, msg : string = reason(status) ) : exn a
  throw(msg, HttpError(status))


// ----------------------------------------------------------------------------
// Parsing
// ----------------------------------------------------------------------------

// Validate the request target (RFC 9112, section 3.2) and return it in origin-form
// (`/path?query`, or `*` for `OPTIONS *`); an absolute-form target (`http://host/path?query`)
// is reduced to its path and query. Rather than normalizing, we reject paths with `.` or `..`
// segments (plain or encoded) and with an encoded `/`, `\`, or NUL.
fun origin-target( method : string, target : string ) : exn string
  if target.is-empty || !target.list.all(fn(c) c >= '!' && c <= '~' && c != '#') then http-error(400, "invalid request target")
  elif target == "*" then
    if method == "OPTIONS" then target else http-error(400, "invalid request target")
  else
    val t = absolute-path(target).default(target)
    if t.starts-with("/").is-nothing then http-error(400, "invalid request target")
    match t.split("?", 2)
      Cons(path, _) -> path.split("/").foreach(check-segment)
      Nil           -> ()
    t

// The path and query of an absolute-form target `http(s)://authority[/path][?query]`.
fun absolute-path( target : string ) : maybe<string>
  match target.split("://", 2)
    Cons(scheme, Cons(rest, Nil)) | scheme.to-lower == "http" || scheme.to-lower == "https" ->
      match rest.split("/", 2)
        Cons(authority, Cons(path, Nil)) | authority.find("?").is-nothing -> Just("/" ++ path)
        _ -> match rest.split("?", 2)
               Cons(_, Cons(query, Nil)) -> Just("/?" ++ query)
               _                         -> Just("/")
    _ -> Nothing

fun check-segment( encoded : string ) : exn ()
  val seg = percent-decode(encoded)
  if encoded == "." || encoded == ".." || seg == "." || seg == ".." || !valid-escapes(encoded.bytes.list) ||
     seg.list.any(fn(c) c == '/' || c == '\\' || c.int == 0) then
    http-error(400, "invalid path segment")

// Is every `%` followed by two hex digits?
fun valid-escapes( xs : list<int> ) : bool
  match xs
    Cons(37, Cons(h, Cons(l, rest))) -> hex-value(h) >= 0 && hex-value(l) >= 0 && valid-escapes(rest)
    Cons(37, _)                      -> False
    Cons(_, rest)                    -> valid-escapes(rest)
    Nil                              -> True

// ----------------------------------------------------------------------------
// Serializing
// ----------------------------------------------------------------------------

// Is a response header valid? The name must be a token, and the value must not contain
// control characters (other than tab): a CR or LF would let a header value that comes
// from user input inject headers or a whole response ("response splitting").
fun valid-header( h : (string,string) ) : bool
  !h.fst.is-empty && h.fst.list.all(is-tchar) &&
  h.snd.list.all(fn(c) (c >= ' ' || c == '\t') && c.int != 127)

fun is-tchar( c : char ) : bool
  c.is-alpha-num || "!#$%&'*+-.^_`|~".list.any(fn(d) d == c)

fun response/serialize( resp : response, keep : bool, head-only : bool ) : bytes
  val status-line = "HTTP/1.1 " ++ resp.status.show ++ " " ++ reason(resp.status) ++ "\r\n"
  val headers = resp.headers.filter(fn(h) { val n = h.fst.to-lower; n != "content-length" && n != "connection" })
                            .map(fn(h) h.fst ++ ": " ++ h.snd ++ "\r\n")
                            .join
  val framing = "Content-Length: " ++ resp.body.length.show ++ "\r\n" ++
                "Connection: " ++ (if keep then "keep-alive" else "close") ++ "\r\n\r\n"
  val head = (status-line ++ headers ++ framing).bytes
  if head-only then head else head ++ resp.body


// ----------------------------------------------------------------------------
// Connections
// ----------------------------------------------------------------------------

// Serve HTTP requests on a connection until it is closed.
fun serve-conn( conn : tcp-conn, app : request -> <async,io> response, cfg : config ) : <async,io> ()
  with finally { conn.close }
  val remote = conn.remote-address
  val parser = new-parser(cfg.max-head, cfg.max-body)

  // Read more input before `deadline` (see `monotonic-ms`); `Nothing` if the deadline passed.
  // (The timeouts are libuv timers in `tcp.c`: cheaper than `timeout` from `std/async`.)
  fun read-before( deadline : int ) : <async,io> maybe<bytes>
    val left = deadline - monotonic-ms()
    if left <= 0 then Nothing else
      match try({ conn.read(left.milli-seconds) })
        Ok(data)   -> Just(data)
        Error(exn) -> if exn.is-timeout then Nothing else throw-exn(exn)

  // Parse `buf[off..]`, reading more input as needed, until the parser completes the request
  // head or the request. Returns the event with the input position after it, or `Nothing`
  // if the client closed the connection (or was idle until `deadline`) between requests.
  fun parse( buf : bytes, off : int, deadline : int ) : <async,io> maybe<(parse-event,bytes,int)>
    val (event, consumed) = parser.execute(buf, off)
    match event
      Need-more -> match read-before(deadline)
        Just(more) | !more.is-empty -> parse(more, 0, deadline)
        Just(_)    -> if parser.in-message then http-error(400, "incomplete request") else Nothing
        Nothing    -> if parser.in-message then http-error(408) else Nothing
      Parse-error(status, msg) -> http-error(status, msg)
      _ -> Just((event, buf, off + consumed))

  // Handle requests; `buf[off..]` holds input that was received but not parsed yet (pipelining).
  fun serve-requests( buf : bytes, off : int ) : <async,io> ()
    match parse(buf, off, monotonic-ms() + cfg.head-timeout.milli-seconds)
      Nothing -> ()   // the client closed the connection, or it was idle for too long
      Just((Head-complete, buf1, off1)) ->
        val method = parser.method
        val target = origin-target(method, parser.url)
        val req0 = Request(method, target, parser.version, parser.headers, empty, remote)
        val hosts = req0.headers.filter(fn(h) h.fst == "host").length
        if hosts > 1 || (hosts == 0 && req0.version == "HTTP/1.1") then http-error(400, "missing or duplicate Host header")
        val len = parser.content-length   // -1 for a chunked body
        if len > cfg.max-body then http-error(413)
        if len != 0 && req0.header("expect").map(to-lower) == Just("100-continue") then
          conn.write-text("HTTP/1.1 100 Continue\r\n\r\n", cfg.write-timeout)
        match parse(buf1, off1, monotonic-ms() + cfg.body-timeout.milli-seconds)
          Just((Message-complete, buf2, off2)) ->
            val req = req0(body = parser.take-body)
            val app-req = if method == "HEAD" then req(method = "GET") else req   // HEAD is answered like GET, without the body
            val resp = match try({ app(app-req) })
                         Ok(resp) | resp.headers.all(valid-header) -> resp
                         Ok(_) ->
                           println("error in handler (" ++ method ++ " " ++ target ++ "): invalid response header")
                           text("Internal Server Error\n", 500)
                         Error(exn) ->
                           println("error in handler (" ++ method ++ " " ++ target ++ "): " ++ exn.message)
                           text("Internal Server Error\n", 500)
            val keep = parser.keep-alive
            conn.write(resp.serialize(keep, method == "HEAD"), cfg.write-timeout)
            if keep then serve-requests(buf2, off2)
                    else close-gracefully(conn)
          _ -> http-error(400, "incomplete request")
      Just(_) -> http-error(400, "invalid request")

  match try({ serve-requests(empty, 0) })
    Ok(_) -> ()
    Error(exn) -> match exn.info
      HttpError(status) ->   // answer the error and close
        val _ = try({ conn.write(text(exn.message ++ "\n", status).serialize(False, False), cfg.write-timeout)
                      close-gracefully(conn) })
        ()
      _ -> ()                // connection errors (reset by peer, timeouts, etc.)

// Close after a response: send a FIN after the pending writes, then read and discard input
// for a while (a "lingering close"). Closing a socket with unread input makes the kernel send
// a reset, which can destroy the response before the client reads it -- e.g. when we answer
// 413 while the client is still sending its body.
fun close-gracefully( conn : tcp-conn ) : <async,io> ()
  val _ = try({
    conn.shutdown(2.seconds)
    drain(conn, 1024 * 1024, monotonic-ms() + 2000)
  })
  ()

// Read and discard up to `budget` bytes, until end-of-stream or `deadline`.
fun drain( conn : tcp-conn, budget : int, deadline : int ) : <async,io> ()
  val left = deadline - monotonic-ms()
  if budget > 0 && left > 0 then
    val data = conn.read(left.milli-seconds)
    if !data.is-empty then drain(conn, budget - data.length, deadline)


// ----------------------------------------------------------------------------
// Server
// ----------------------------------------------------------------------------

// Serve HTTP on a listening socket: every connection runs in its own strand.
// Returns after the server is closed (`srv.close`) and all connections are done.
pub fun serve-on( srv : tcp-server, app : request -> <async,io> response, cfg : config = Config() ) : <async,io> ()
  nursery fn(spawn)
    fun accept-loop()
      match try({ srv.accept })
        Ok(conn) ->
          spawn{ serve-conn(conn, app, cfg) }
          accept-loop()
        Error(exn) ->
          if srv.is-closed then () else   // once the server is closed, we stop accepting
            println("accept failed: " ++ exn.message)
            wait(50.milli-seconds)        // e.g. out of file descriptors: back off a little
            accept-loop()
    accept-loop()

// Listen on `host:port` and serve HTTP.
pub fun serve( host : string, port : int, app : request -> <async,io> response, cfg : config = Config() ) : <async,io> ()
  serve-on(listen(host, port), app, cfg)
