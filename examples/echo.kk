// TCP echo server: `koka -e examples/echo.kk`, then `nc 127.0.0.1 9000`
import std/async
import tcp
import nursery

fun main()
  val srv = listen("127.0.0.1", 9000)
  println("echo server on " ++ srv.local-address)
  nursery fn(spawn)
    fun accept-loop()
      val conn = srv.accept
      spawn fn()
        with finally { conn.close }
        fun echo()
          val data = conn.read
          if !data.is-empty then
            conn.write(data)
            echo()
        echo()
      accept-loop()
    accept-loop()
