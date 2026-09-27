// Server with short timeouts, for testing: head/body 700ms, write 1s; /big sends 64 MiB.
import std/time/duration
import std/async
import tcp
import http

fun app( req : request ) : <async,io> response
  match req.segments
    ["big"] -> Response(200, [("Content-Type", "application/octet-stream")], bytes(64 * 1024 * 1024))
    _       -> text("ok\n")

fun main()
  val srv = listen("127.0.0.1", 18090)
  serve-on(srv, app, Config(head-timeout = 700.milli-seconds, body-timeout = 700.milli-seconds, write-timeout = 1.seconds))
