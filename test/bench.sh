#!/bin/sh
# Benchmark the optimized example server with ApacheBench: `mise run bench` (needs ab and python3).
# Uses port 18180. ab runs on the same machine and is itself single-threaded.
set -u
cd "$(dirname "$0")/.."
ulimit -n 10240 2>/dev/null || true

port=18180
url="http://127.0.0.1:$port/hello/Koka"
.koka/web-release $port > /dev/null 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
sleep 0.5

summary() {  # extract requests/s, failures, and the median and 99th percentile latency from ab's output
  awk '/Failed requests/ { failed = $3 }
       /Requests per second/ { rps = $4 }
       /^ *50%/ { p50 = $2 }
       /^ *99%/ { p99 = $2 }
       END { printf "%8.0f req/s   median %s ms   p99 %s ms   failed %s\n", rps, p50, p99, failed }'
}

ab -q -k -n 20000 -c 50 "$url" > /dev/null   # warm up

printf "keep-alive, 50 connections          "; ab -q -k -n 200000 -c 50 "$url" | summary
printf "new connection per request, 50      "; ab -q -n 50000 -c 50 "$url" | summary

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
printf "keep-alive, 50 + 2000 idle          "; ab -q -k -n 100000 -c 50 "$url" | summary
echo "server memory (RSS) with 2000+ connections: $(ps -o rss= -p $pid | tr -d ' ') KB"
wait $idle
