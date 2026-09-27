// Graceful-shutdown pattern: stop serving when /shutdown is requested.
import std/async
import tcp
import http

fun main()
  val srv = listen("127.0.0.1", 18215)
  val stop : channel<()> = channel()
  fun app( req : request ) : <async,io> response
    if req.path == "/shutdown" then { stop.emit(()); text("bye\n") } else text("ok\n")
  firstof { serve-on(srv, app) } { stop.receive(); trace("shutdown requested") }
  trace("main done")
