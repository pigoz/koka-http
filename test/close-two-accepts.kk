// Two strands wait in `accept` on the same server; a third closes the server.
import std/time/duration
import std/async
import tcp

fun main()
  val srv = listen("127.0.0.1", 18101)
  println("listening")
  val r = try {
    interleaved([
      { val _ = srv.accept; () },
      { val _ = srv.accept; () },
      { wait(100.milli-seconds); srv.close }
    ])
    ()
  }
  match r
    Ok(_)    -> println("ok")
    Error(e) -> println("error: " ++ e.message)
  println("done")
