// std/async only: an exception inside `timeout` should propagate right away.
import std/time/duration
import std/time/timer
import std/async

fun main()
  val t0 = ticks()
  val r = try({ timeout(2.seconds){ wait(10.milli-seconds); throw("boom") } })
  val dt = (ticks() - t0).milli-seconds
  match r
    Ok(Nothing)  -> println("returned Nothing (timed out) after ~" ++ dt.show ++ "ms  <- the exception was lost")
    Ok(Just(_))  -> println("returned a value?!")
    Error(exn)   -> println("exception '" ++ exn.message ++ "' after ~" ++ dt.show ++ "ms")
