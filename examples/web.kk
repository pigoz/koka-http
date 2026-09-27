// Example web server: `mise run serve` (or `mise run serve -- 8081`), then
//   curl http://127.0.0.1:8080/hello/Koka
import std/os/env
import std/time/duration
import std/async
import tcp
import http

fun app( req : request ) : <async,io> response
  match (req.method, req.segments)
    ("GET", [])              -> html("<!doctype html><h1>Hello from Koka!</h1><p>You are " ++ req.remote ++ "</p>\n")
    ("GET", ["hello"])       -> text("Hello, world!\n")
    ("GET", ["hello", name]) | !name.is-empty -> text("Hello, " ++ name ++ "!\n")
    ("GET", ["slow"])        ->
      wait(1.seconds)        // other requests keep being served meanwhile
      text("that took a second\n")
    ("POST", ["echo"])       ->
      Response(200, [("Content-Type", req.header("content-type").default("application/octet-stream"))], req.body)
    ("GET", ["boom"])        -> throw("boom")   // answered with a 500
    _                        -> text("Not Found\n", 404)

fun main()
  val port = match get-args()
               Cons(arg, _) -> arg.parse-int.default(8080)
               Nil          -> 8080
  val srv = listen("127.0.0.1", port)
  println("listening on http://" ++ srv.local-address)
  serve-on(srv, app)
