import socket, sys, time
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8098
def send(raw, read_timeout=2.0, chunks=None, delay=0.0):
    s = socket.create_connection(("127.0.0.1", PORT)); s.settimeout(read_timeout)
    if chunks:
        for c in chunks: s.sendall(c); time.sleep(delay)
    else:
        s.sendall(raw)
    data = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            data += d
    except socket.timeout:
        data += b"<timeout>"
    except ConnectionResetError:
        data += b"<reset>"
    s.close()
    return data
import re
def statuses(data):
    return [m.decode() for m in re.findall(rb"HTTP/1\.1 (\d{3}) ", data) if m != b"100"]
def show(name, raw, expect, **kw):
    data = send(raw, **kw)
    st = statuses(data)
    closed = not data.endswith(b"<timeout>")
    if data.endswith(b"<reset>"): st.append("RESET")
    ok = st == expect[0] and (expect[1] is None or closed == expect[1])
    print(f"{'OK ' if ok else 'BAD'} {name:44} statuses={st} closed={closed}" + ("" if ok else f"   expected={expect}  data={data[:300]!r}"))
C = b"Connection: close\r\n"
show("GET /hello/Koka", b"GET /hello/Koka HTTP/1.1\r\nHost: x\r\n"+C+b"\r\n", (["200"], True))
show("POST echo Content-Length", b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"+C+b"\r\nhello", (["200"], True))
d = send(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"+C+b"\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
print(("OK " if d.endswith(b"hello world") else "BAD") + " POST echo chunked -> body " + repr(d.split(b"\r\n\r\n",1)[-1]))
show("pipelined x3 (keep-alive, last close)", b"GET /hello/a HTTP/1.1\r\nHost: x\r\n\r\nGET /hello/b HTTP/1.1\r\nHost: x\r\n\r\nGET /hello/c HTTP/1.1\r\nHost: x\r\n"+C+b"\r\n", (["200","200","200"], True))
req = b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabcGET /hello/z HTTP/1.1\r\nHost: x\r\n"+C+b"\r\n"
show("pipelined, split byte by byte", None, (["200","200"], True), chunks=[bytes([b]) for b in req], delay=0.002)
show("HEAD", b"HEAD /hello/Koka HTTP/1.1\r\nHost: x\r\n"+C+b"\r\n", (["200"], True))
show("HTTP/1.0 (closes)", b"GET /hello HTTP/1.0\r\n\r\n", (["200"], True))
show("HTTP/1.0 keep-alive (stays open)", b"GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", (["200"], False), read_timeout=1)
show("HTTP/1.1 idle keep-alive (stays open)", b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", (["200"], False), read_timeout=1)
d = send(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n"+C+b"\r\n", read_timeout=1)
print(("OK " if d.startswith(b"HTTP/1.1 100 Continue\r\n\r\n") else "BAD") + " Expect: 100-continue -> " + repr(d[:30]))
print("--- smuggling / framing")
show("CL + TE chunked", b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", (["400"], True))
show("duplicate CL (different)", b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 5\r\n\r\nhello", (["400"], True))
show("duplicate CL (same)", b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello", (["400"], True))
for v in [b"+5", b"0x5", b"1_0", b"5 5", b"-1", b"5e0", b"99999999999999999999999"]:
    show("CL " + v.decode(), b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: " + v + b"\r\n\r\nhello", (["400"], True))
show("TE chunked, gzip (chunked not last)", b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n0\r\n\r\n", (["400"], True))
show("TE xchunked", b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: xchunked\r\n\r\n0\r\n\r\n", (["400"], True))
show("space before colon", b"GET / HTTP/1.1\r\nHost : x\r\n\r\n", (["400"], True))
show("obs-fold", b"GET / HTTP/1.1\r\nHost: x\r\nX-A: a\r\n b\r\n\r\n", (["400"], True))
show("bare LF", b"GET / HTTP/1.1\nHost: x\n\n", (["400"], True))
show("NUL in header value", b"GET / HTTP/1.1\r\nHost: x\x00y\r\n\r\n", (["400"], True))
show("invalid chunk size", b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n", (["400"], True))
show("HTTP/1.1 without Host", b"GET / HTTP/1.1\r\n\r\n", (["400"], True))
show("duplicate Host", b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", (["400"], True))
print("--- limits and odd requests")
show("huge header (>16KiB)", b"GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + b"a"*20000 + b"\r\n\r\n", (["431"], True))
show("101 headers", b"GET / HTTP/1.1\r\n" + b"".join(b"X-H%d: v\r\n" % i for i in range(101)) + b"\r\n", (["431"], True))
show("CL 2MB (413 before body)", b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n", (["413"], True))
show("chunked > 1MiB (413)", b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + (b"10000\r\n" + b"a"*65536 + b"\r\n")*17 + b"0\r\n\r\n", (["413"], True))
show("unknown method FOO", b"FOO / HTTP/1.1\r\nHost: x\r\n\r\n", (["501"], True))
show("garbage (TLS hello)", b"\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", (["501"], True))
show("HTTP/2 preface", b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", (["505"], True))
show("HTTP/1.2", b"GET / HTTP/1.2\r\nHost: x\r\n\r\n", (["400"], True))
show("upgrade websocket (served, then closed)", b"GET /hello HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n", (["200"], True))
show("CONNECT", b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n", (["400"], True))
show("absolute-form", b"GET http://example.com/hello/Abs HTTP/1.1\r\nHost: example.com\r\n"+C+b"\r\n", (["200"], True))
show("/hello/..%2Fetc", b"GET /hello/..%2Fetc HTTP/1.1\r\nHost: x\r\n\r\n", (["400"], True))
