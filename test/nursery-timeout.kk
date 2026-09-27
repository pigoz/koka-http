// Does a `timeout` inside one spawned strand disturb the other strands?
import std/time/duration
import std/async
import nursery

fun main()
  nursery fn(spawn)
    spawn fn()
      println("A: waiting 300ms")
      wait(300.milli-seconds)
      println("A: done (should print)")
    spawn fn()
      val r = timeout(1.seconds){ wait(50.milli-seconds); "fast" }
      println("B: timeout result: " ++ r.default("timed out"))
    println("body: done")
  println("nursery done")
