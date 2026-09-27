// Same as outer-cancel1 but count finalizers with a global ref and report at the end.
import std/time/duration
import std/async
import nursery

fun main()
  val fin = ref(0)
  val r = timeout(300.milli-seconds)
    nursery fn(spawn)
      spawn fn()
        with finally { fin := !fin + 1 }
        wait(5.seconds)
      spawn fn()
        with finally { fin := !fin + 1 }
        wait(10.milli-seconds)
        val _ = timeout(10.seconds){ wait(5.seconds); "x" }
        ()
    42
  println("outer timeout result: " ++ r.map(show).default("timed out") ++ ", finalizers run: " ++ (!fin).show ++ " of 2")
  wait(500.milli-seconds)
  println("finalizers run after 500ms: " ++ (!fin).show ++ " of 2")
