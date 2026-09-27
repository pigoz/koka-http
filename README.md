# koka-http

A small HTTP/1.1 server written in [Koka](https://koka-lang.github.io), on top of the new
`std/async` library (libuv) of Koka 3.2.9, with requests parsed by
[llhttp](https://github.com/nodejs/llhttp).

```koka
fun app( req : request ) : <async,io> response
  match (req.method, req.segments)
    ("GET", [])                               -> html("<h1>Hello from Koka!</h1>")
    ("GET", ["hello", name]) | !name.is-empty -> text("Hello, " ++ name ++ "!\n")
    ("POST", ["echo"])                        -> Response(200, [], req.body)
    _                                         -> text("Not Found\n", 404)

fun main()
  serve-on(listen("127.0.0.1", 8080), app)
```

- one strand per connection; keep-alive and pipelining
- `Content-Length` and chunked request bodies, `Expect: 100-continue`
- strict request framing (llhttp) and request targets (no `..`, no encoded `/`)
- limits and timeouts for heads, bodies, and writes (libuv timers)
- graceful shutdown: canceling the server closes its connections

## Usage

Needs [mise](https://mise.jdx.dev) (it installs Koka 3.2.9 and sets the C include paths):

```sh
mise run serve          # build and run examples/web.kk on port 8080
mise run test           # tests (needs python3)
mise run test-asan      # the same tests on an AddressSanitizer build
mise run bench          # benchmark the optimized build
mise run release        # optimized build in .koka/web-release
mise run echo           # TCP echo server on port 9000
```

Only tried on macOS (arm64).

## Testing

`mise run test` checks, against the running example server:

- **the protocol** (41 checks): keep-alive, pipelining (also fed byte by byte), `HEAD`,
  `Expect: 100-continue`, chunked bodies; the usual request smuggling vectors are rejected with
  400 (`Content-Length` together with `Transfer-Encoding`, duplicate or non-numeric
  `Content-Length` such as `+5`, `0x5`, or `1e3`, `chunked` that is not the last coding, bare LF,
  obs-fold, missing or duplicate `Host`); request targets with `..`, an encoded `/`, `\`, or NUL,
  or invalid escapes; limits (413, 431); unknown methods, TLS and HTTP/2 prefaces, `CONNECT`,
  upgrades
- **abrupt disconnects**: 2000 clients that reset or half-close in every phase (before the
  request, in the head, in the body, during a slow handler, during a 1 MB response)
- **timeouts**: idle keep-alive connections, slow heads and bodies (408), and a client that
  stops reading (the write timeout closes the connection)
- **cancelation**: a timeout in one connection does not affect the others, closing a server with
  pending accepts, and a graceful shutdown in which open keep-alive connections get closed

`mise run test-asan` passes as well: AddressSanitizer reports no memory errors. (Checked
separately: under repeated rounds of load and abrupt disconnects, memory use and open file
descriptors stay flat.)

The code also got an adversarial review (reference counting across the Koka/C boundary, libuv
handle lifecycles, cancelation, HTTP). The problems it found, like a use-after-free when closing
a server with several pending accepts or the missing write timeout, are fixed here.

## Performance

`mise run bench`: optimized build, load from [oha](https://github.com/hatoo/oha) on the same
machine (Apple M1 Max, macOS 15.3).

| | requests/s | p50 | p99 |
|---|---:|---:|---:|
| keep-alive, 50 connections | 50,000–58,000 | 0.84 ms | 1–3 ms |
| keep-alive, 200 connections | ~47,000 | 4.2 ms | 5.1 ms |
| a new connection per request, 50 at a time | ~22,000 | 2.2 ms | 4.5 ms |
| keep-alive, 50 connections, plus 2000 idle ones | ~57,000 | 0.85 ms | 1.5 ms |

The server is single-threaded and these numbers are bound by it: under keep-alive load it uses
one core fully (ApacheBench gives the same results). With more *active* connections each request
costs a bit more: profiling shows the time going into the list of outstanding awaits of
`std/async`, whose operations are linear in the number of active awaits. Idle connections cost
nothing: with 2000 of them open the server uses about 15 MB of memory (RSS). The rate of new
connections is measured in short bursts: on loopback, a sustained rate soon runs out of ports
(`TIME_WAIT`).

## Layout

| | |
|---|---|
| `src/tcp.kk`, `src/tcp.c` | TCP streams for `std/async`: libuv bindings |
| `src/httpparse.kk`, `src/httpparse.c`, `src/llhttpcore.kk` | request parsing with llhttp |
| `src/http.kk` | the HTTP server |
| `src/nursery.kk` | dynamically spawned strands (one per connection) |
| `examples/` | the example web server and a TCP echo server |
| `test/` | tests (`test/run.sh`) |
| `vendor/` | libuv 1.52.1 headers (matching the libuv bundled with Koka 3.2.9) and llhttp 9.4.3 |

## Notes

`src/nursery.kk` works around some problems in `std/async` of Koka 3.2.9; fixes are proposed
upstream in [#933](https://github.com/koka-lang/koka/pull/933),
[#934](https://github.com/koka-lang/koka/pull/934), and
[#935](https://github.com/koka-lang/koka/pull/935). Also see
[#936](https://github.com/koka-lang/koka/pull/936) (`parse-int`), and
[#268](https://github.com/koka-lang/koka/issues/268): Koka does not rebuild a module when only
its included C file changed, so the mise tasks touch the `.kk` file first.

The vendored libuv headers and llhttp come with their own (MIT) licenses in `vendor/`.
