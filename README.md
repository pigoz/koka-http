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
mise run bench          # benchmark the optimized build (needs ab)
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

`mise run bench`: optimized build, [ApacheBench](https://httpd.apache.org/docs/2.4/programs/ab.html)
on the same machine (Apple M1 Max, macOS 15.3). The server is single-threaded, and so is `ab`,
which limits these numbers too.

| | requests/s | median | p99 |
|---|---:|---:|---:|
| keep-alive, 50 connections | ~60,000 | 1 ms | 1 ms |
| a new connection per request, 50 at a time | ~27,500 | 2 ms | 2–3 ms |
| keep-alive, 50 connections, plus 2000 idle ones | ~59,000 | 1 ms | 1–2 ms |

With 2000 open connections the server uses about 15 MB of memory (RSS).

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
