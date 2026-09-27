#!/bin/sh
# Run the tests: `mise run test` (needs python3; uses ports 18090-18099 and 18215).
#
# Not run here, since they demonstrate bugs in std/async of Koka 3.2.9 that our code works around:
#   test/upstream-nested-timeout.kk     a `timeout` in an `interleaved` strand cancels its siblings
#   test/upstream-timeout-exception.kk  `timeout` loses an exception and waits for the full duration
set -u
cd "$(dirname "$0")/.."
failed=0

pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1"; failed=$((failed + 1)); }

build() {  # file output (extra compiler flags can be given in TEST_KOKA_FLAGS, see `mise run test-asan`)
  if ! koka ${TEST_KOKA_FLAGS:-} -isrc -c "$1" -o "$2" > .koka/test-build.log 2>&1; then
    cat .koka/test-build.log
    fail "build $1"
    return 1
  fi
}

# Koka programs whose output must contain a given line
check_output() {  # name expected
  if build "test/$1.kk" ".koka/test-$1"; then
    out=$(".koka/test-$1" 2>&1)
    if echo "$out" | grep -q -F "$2"; then pass "$1"; else echo "$out"; fail "$1"; fi
  fi
}

mkdir -p .koka
for c in src/*.c; do kk="${c%.c}.kk"; if [ "$c" -nt "$kk" ]; then touch "$kk"; fi; done

check_output nursery-timeout      "A: done (should print)"
check_output nursery-outer-cancel "finalizers run: 2 of 2"
check_output close-two-accepts    "done"

# HTTP protocol probes and abrupt disconnects against the example server
if build examples/web.kk .koka/test-web; then
  .koka/test-web 18091 > .koka/test-web.log 2>&1 &
  pid=$!
  sleep 0.5
  out=$(python3 test/probe.py 18091 2>&1)
  if echo "$out" | grep -q '^BAD'; then echo "$out" | grep '^BAD'; fail "http probes"; else pass "http probes ($(echo "$out" | grep -c '^OK') checks)"; fi
  python3 test/disconnects.py 18091 > .koka/test-disconnects.log 2>&1
  if curl -s -m 3 http://127.0.0.1:18091/hello/again | grep -q 'Hello, again!'; then pass "abrupt disconnects"; else fail "abrupt disconnects"; fi
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
fi

# timeouts (head/body 700ms, write 1s)
if build test/short-timeouts.kk .koka/test-short-timeouts; then
  .koka/test-short-timeouts > .koka/test-short-timeouts.log 2>&1 &
  pid=$!
  sleep 0.5
  out=$(python3 - <<'EOF'
import socket, time
def conn():
    s = socket.create_connection(("127.0.0.1", 18090)); s.settimeout(5); return s
def recv_all(s):
    data = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            data += d
    except Exception: pass
    return data
t0 = time.time(); s = conn(); s.sendall(b"GET /x HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(4096); recv_all(s)
print("idle", round(time.time() - t0, 1))
t0 = time.time(); s = conn(); s.sendall(b"GET / HTTP/1.1\r\nHo"); d = recv_all(s)
print("head", d.split(b" ")[1].decode() if d else "none", round(time.time() - t0, 1))
t0 = time.time(); s = conn(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096); s.sendall(b"GET /big HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(2.5); d = recv_all(s)
print("slow-reader", "closed" if len(d) < 64 * 1024 * 1024 else "not-closed")
EOF
)
  if echo "$out" | grep -q '^idle 0.7' && echo "$out" | grep -q '^head 408 0.7' && echo "$out" | grep -q '^slow-reader closed'; then
    pass "timeouts"
  else
    echo "$out"; fail "timeouts"
  fi
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
fi

# graceful shutdown: keep-alive clients get EOF and the process exits
if build test/shutdown-endpoint.kk .koka/test-shutdown-endpoint; then
  .koka/test-shutdown-endpoint > .koka/test-shutdown.log 2>&1 &
  pid=$!
  sleep 0.5
  out=$(python3 - <<'EOF'
import socket
conns = []
for i in range(3):
    s = socket.create_connection(("127.0.0.1", 18215)); s.settimeout(5)
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(4096); conns.append(s)
s = socket.create_connection(("127.0.0.1", 18215)); s.sendall(b"GET /shutdown HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(4096)
print("eof" if all(c.recv(4096) == b"" for c in conns) else "no-eof")
EOF
)
  sleep 1
  if [ "$out" = "eof" ] && ! kill -0 $pid 2>/dev/null; then pass "graceful shutdown"; else echo "$out"; kill $pid 2>/dev/null; fail "graceful shutdown"; fi
fi

if [ $failed -eq 0 ]; then echo "all tests passed"; else echo "$failed test(s) failed"; exit 1; fi
