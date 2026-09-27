// std/async only: a `timeout` inside an `interleaved` strand, after that strand already awaited once.
import std/time/duration
import std/async

fun main()
  interleaved {
    println("A: waiting 300ms")
    wait(300.milli-seconds)
    println("A: done (should print)")
  } {
    wait(10.milli-seconds)   // after this, B is resumed from the interleave loop
    val r = timeout(1.seconds){ wait(50.milli-seconds); "fast" }
    println("B: timeout result: " ++ r.default("timed out"))
  }
  println("main done")
