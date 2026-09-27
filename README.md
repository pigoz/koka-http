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
mise run release        # optimized build in .koka/web-release
mise run echo           # TCP echo server on port 9000
```

Only tried on macOS (arm64).

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

`src/nursery.kk` works around some problems in `std/async` of Koka 3.2.9, which are fixed
upstream by [#933](https://github.com/koka-lang/koka/pull/933),
[#934](https://github.com/koka-lang/koka/pull/934), and
[#935](https://github.com/koka-lang/koka/pull/935). Also see
[#936](https://github.com/koka-lang/koka/pull/936) (`parse-int`), and
[#268](https://github.com/koka-lang/koka/issues/268): Koka does not rebuild a module when only
its included C file changed, so the mise tasks touch the `.kk` file first.

The vendored libuv headers and llhttp come with their own (MIT) licenses in `vendor/`.
