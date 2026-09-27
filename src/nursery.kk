/*---------------------------------------------------------------------------
  Dynamically spawned strands.

  `std/async` offers `interleaved` over a fixed list of actions; a server needs
  to start a new strand per connection. A `nursery` does this with the same
  technique as `interleave.kk`: every await in a strand is turned into a
  "no-await" whose completion queues the strand's resumption, and the nursery
  runs the queued resumptions until all strands are done.

  This also works around three problems of `std/async` in Koka 3.2.9:
  - Cancelation scopes: each strand gets its own scope (a child of the nursery
    scope), installed _inside_ the strand so it is part of the strand's
    continuation. In `interleave.kk` the scope handler is outside the strand,
    so after a strand is resumed, `cancel-outstanding` (used by `timeout` and
    `firstof`) cancels the scope of whoever resumed it: a `timeout` in one
    connection would cancel all the others.
  - Outer cancelation: when the nursery is canceled from outside, it cancels
    its strands and keeps running them until they are done (so their
    finalizers run and sockets get closed), and then propagates the
    cancelation. (Using a channel, as `interleave.kk` does, the loop's own
    `receive` is canceled first and the strands are abandoned.)
  - Duplicate callbacks: a canceled `channel.receive` leaves a stale listener
    behind, which can call back an await a second time. We ignore a second
    callback for the same await.
---------------------------------------------------------------------------*/
module nursery

import std/time/duration
import std/async
import std/async/internal

alias resumption = () -> <async,io> ()

// Run `body` with a `spawn` function that starts new strands, interleaved with
// `body` and each other. Returns when `body` and all spawned strands are done.
// An exception in a strand is passed to `on-error` and does not affect other strands.
// When the nursery is canceled, all its strands are canceled (and their finalizers run).
pub fun nursery( body : (spawn : (() -> <async,io> ()) -> <async,io> ()) -> <async,io> (),
                 on-error : exception -> <async,io> () = report-error ) : <async,io> ()
  // queue of resumptions to run: `front` in order, then `back` reversed
  val front : ref<global,list<resumption>> = ref([])
  val back  : ref<global,list<resumption>> = ref([])
  val waker : ref<global,maybe<() -> ioc ()>> = ref(Nothing)   // wakes up the loop when it waits
  val active = ref(0)
  val canceled = ref(False)
  val nursery-scope = new-child-scope(async-scope)

  fun push( r : resumption ) : ioc ()
    back := Cons(r, !back)
    match waker.swap(Nothing)
      Just(wake) -> wake()
      Nothing    -> ()

  fun pop() : ioc maybe<resumption>
    match !front
      Cons(r, rs) ->
        front := rs
        Just(r)
      Nil -> match (!back).reverse
        Cons(r, rs) ->
          back := []
          front := rs
          Just(r)
        Nil -> Nothing

  // The next resumption to run, or `Nothing` if waiting for it was canceled.
  fun next() : <async,io> maybe<resumption>
    match async-ioc(pop)
      Just(r) -> Just(r)
      Nothing ->
        val woken = unsafe-uncancel
          val _ = setup/await-error("nursery") fn(_evloop, resume)
            waker := Just(fn() resume(()))
            Ok(fn() waker := Nothing)    // on cancelation, leave no stale waker behind
          ()
        match woken
          Just(_) -> next()
          Nothing -> Nothing

  fun spawn( action : () -> <async,io> () ) : <async,io> ()
    active := !active + 1
    val strand-scope = new-child-scope(nursery-scope)
    with queue-on-await(push)        // awaits queue their resumption instead of blocking
    with pin-scope(strand-scope)     // inside the strand: survives resumptions
    val _ = unsafe-uncancel            // a canceled strand just ends
      match try({ action() })
        Error(exn) -> on-error(exn)
        Ok(_)      -> ()
    active := !active - 1

  spawn{ body(spawn) }
  while { !active > 0 }
    match next()
      Just(resume) -> resume()
      Nothing ->
        // we were canceled from outside: cancel our strands, and keep running them until they are done
        if !canceled then () else
          canceled := True
          cancel-scope(nursery-scope)
  if !canceled then discontinue()

// Like `emit-on-await` in `std/async/channel`: an await in `action` does not block but
// queues the resumption with `push` once it completes (a second callback is ignored).
fun queue-on-await( push : resumption -> ioc (), action : () -> <async,io> () ) : <async,io> ()
  override handle<async-await>(action)
    raw ctl do-await(setup, scope, label)
      val fired = ref(False)
      no-await(setup, scope, "nursery: " ++ label) fn(res)
        if !fired then () else
          fired := True
          push(fn() rcontext.resume(res))
      ()
    fun no-await(setup, scope, label, f) no-await(setup, scope, label, f)

// Run `action` in the cancelation scope `scope`. Cancelation itself is done by the
// outermost async handler (it cancels every outstanding await whose scope is in the
// canceled scope), so we just forward the operations.
fun pin-scope( scope : async-scope, action : () -> <async|e> a ) : <async|e> a
  override handle<async-cancel>(action)
    val async-scope = scope
    fun is-scope-canceled(s) is-scope-canceled(s)
    fun cancel-scope(s)      cancel-scope(s)

// Run `action` with a time limit: returns `Nothing` if it did not finish within `d`.
// Use this instead of `timeout` from `std/async`: in Koka 3.2.9 that one loses
// exceptions (it waits for the full duration and then returns `Nothing`); here the
// exception is caught inside the time limit and rethrown outside.
pub fun with-timeout( d : duration, action : () -> <async,io> a ) : <async,io> maybe<a>
  val res = timeout(d){ try(action) }
  match res
    Just(r) -> Just(r.untry)
    Nothing -> Nothing

fun report-error( exn : exception ) : <async,io> ()
  println("error: " ++ exn.message)
