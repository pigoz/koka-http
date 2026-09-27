/*---------------------------------------------------------------------------
  Incremental HTTP/1.1 request parsing with llhttp (see `httpparse.c`).

  Feed input with `execute`: it stops when the request head is complete
  (`Head-complete`), when the request is complete (`Message-complete`), or when
  it needs more input (`Need-more`); the number of consumed bytes is returned
  so the rest of the input can be fed again later (pipelining).
---------------------------------------------------------------------------*/
module httpparse

import std/num/int32
import std/num/int64
import llhttpcore

extern import
  c file "httpparse.c"

// llhttp itself (vendor/llhttp, v9.4.3); `llhttp.h` comes from the include path (see mise.toml),
// and the state machine is compiled separately in `llhttpcore`.
extern import
  c file "../vendor/llhttp/src/api.c"
extern import
  c file "../vendor/llhttp/src/http.c"

// A request parser (one per connection).
abstract struct parser( hp : any )

// What `execute` stopped at.
pub type parse-event
  // All input was consumed; feed more.
  Need-more
  // The request line and headers are complete (the body follows).
  Head-complete
  // The request is complete (including the body).
  Message-complete
  // The input is not a valid request: answer with `status` and close the connection.
  Parse-error( status : int, reason : string )

extern prim-new( max-head : ssize_t, max-body : ssize_t ) : ioc any
  c "kk_hp_new"
extern prim-execute( ^hp : any, ^data : bytes, from : ssize_t ) : ioc int32
  c "kk_hp_execute"
extern prim-consumed( ^hp : any ) : ioc ssize_t
  c "kk_hp_consumed"
extern prim-in-message( ^hp : any ) : ioc bool
  c "kk_hp_in_message"
extern prim-error-status( ^hp : any ) : ioc int32
  c "kk_hp_error_status"
extern prim-error-reason( ^hp : any ) : ioc string
  c "kk_hp_error_reason"
extern prim-method( ^hp : any ) : ioc string
  c "kk_hp_method"
extern prim-url( ^hp : any ) : ioc string
  c "kk_hp_url"
extern prim-version( ^hp : any ) : ioc string
  c "kk_hp_version"
extern prim-header-count( ^hp : any ) : ioc ssize_t
  c "kk_hp_header_count"
extern prim-header-name( ^hp : any, i : ssize_t ) : ioc string
  c "kk_hp_header_name"
extern prim-header-value( ^hp : any, i : ssize_t ) : ioc string
  c "kk_hp_header_value"
extern prim-content-length( ^hp : any ) : ioc int64
  c "kk_hp_content_length"
extern prim-keep-alive( ^hp : any ) : ioc bool
  c "kk_hp_keep_alive"
extern prim-take-body( ^hp : any ) : ioc bytes
  c "kk_hp_take_body"

// A new parser; heads larger than `max-head` bytes fail with 431, bodies larger than `max-body` with 413.
pub fun new-parser( max-head : int, max-body : int ) : ioc parser
  Parser(prim-new(max-head.ssize_t, max-body.ssize_t))

// Parse `data` from index `from`. Returns what the parser stopped at, and how many bytes it consumed.
pub fun execute( p : parser, data : bytes, from : int = 0 ) : ioc (parse-event, int)
  val code = prim-execute(p.hp, data, from.ssize_t).int
  val consumed = prim-consumed(p.hp).int
  val event = match code
                0 -> Need-more
                1 -> Head-complete
                2 -> Message-complete
                _ -> Parse-error(prim-error-status(p.hp).int, prim-error-reason(p.hp))
  (event, consumed)

// Is a request partially received (so the end of the stream now would cut it off)?
pub fun in-message( p : parser ) : ioc bool
  prim-in-message(p.hp)

// The method of the current request (e.g. "GET").
pub fun method( p : parser ) : ioc string
  prim-method(p.hp)

// The request target, as sent.
pub fun url( p : parser ) : ioc string
  prim-url(p.hp)

// The protocol version (e.g. "HTTP/1.1").
pub fun version( p : parser ) : ioc string
  prim-version(p.hp)

// The headers, with lower-case names and trimmed values, in order.
pub fun headers( p : parser ) : ioc list<(string,string)>
  val n = prim-header-count(p.hp).int
  list(0, n - 1).map fn(i)
    (prim-header-name(p.hp, i.ssize_t).to-lower, prim-header-value(p.hp, i.ssize_t).trim)

// The declared body length (`Content-Length`), or -1 for a chunked body.
pub fun content-length( p : parser ) : ioc int
  prim-content-length(p.hp).int

// Can the connection be reused after the current request?
pub fun keep-alive( p : parser ) : ioc bool
  prim-keep-alive(p.hp)

// The body of the completed request (can be taken once).
pub fun take-body( p : parser ) : ioc bytes
  prim-take-body(p.hp)
