#!/bin/sh
# Benchmark the optimized example server with oha: `mise run bench` (needs python3).
# Uses port 18180. The server is single-threaded; oha runs on the same machine.
set -u
cd "$(dirname "$0")/.."
ulimit -n 10240 2>/dev/null || true

port=18180
url="http://127.0.0.1:$port/hello/Koka"
.koka/web-release $port > /dev/null 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
sleep 0.5

run() {  # label oha-arguments...
  label=$1
  shift
  printf "%-48s" "$label"
  oha --no-tui --output-format json "$@" "$url" 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin); s = d["summary"]; p = d["latencyPercentiles"]
print("%8.0f req/s   p50 %5.2f ms   p99 %5.2f ms   ok %5.1f%%" % (s["requestsPerSec"], p["p50"] * 1000, p["p99"] * 1000, s["successRate"] * 100))'
}

oha --no-tui -z 2s -c 50 "$url" > /dev/null 2>&1   # warm up

run "keep-alive, 50 connections"                 -z 10s -c 50
run "keep-alive, 200 connections"                -z 10s -c 200
# a short burst: on loopback, a sustained rate of new connections runs out of ports (TIME_WAIT)
run "new connection per request, 50 at a time"   -n 20000 -c 50 --disable-keepalive

# 2000 idle keep-alive connections (they stay open for the 10 s head timeout)
python3 - "$port" <<'EOF' &
import socket, sys, time, resource
resource.setrlimit(resource.RLIMIT_NOFILE, (10240, 10240))
conns = []
for i in range(2000):
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
    s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")
    conns.append(s)
for s in conns: s.recv(4096)
time.sleep(8)
EOF
idle=$!
sleep 2
run "keep-alive, 50 connections + 2000 idle"     -z 5s -c 50
echo "server memory (RSS) with 2000+ connections: $(ps -o rss= -p $pid | tr -d ' ') KB"
wait $idle
