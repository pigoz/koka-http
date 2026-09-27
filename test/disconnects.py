import socket, struct, threading, time, random, sys
PORT = int(sys.argv[1])
def rst(s):
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); s.close()
def c(): return socket.create_connection(("127.0.0.1", PORT))
scen = 0
def run(i):
    global scen
    k = i % 8
    try:
        s = c()
        if k == 0: rst(s)                                                       # connect, reset
        elif k == 1: s.sendall(b"GET /hel"); rst(s)                              # mid-head reset
        elif k == 2: s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n" + b"a"*5000); rst(s)  # mid-body
        elif k == 3: s.sendall(b"GET /slow HTTP/1.1\r\nHost: x\r\n\r\n"); time.sleep(0.05); rst(s)   # during handler
        elif k == 4: s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n" + b"b"*1000000); time.sleep(0.01); rst(s)  # during big response
        elif k == 5: s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"); s.shutdown(socket.SHUT_WR); s.settimeout(3); assert s.recv(100).startswith(b"HTTP/1.1 200"); s.close()  # half-close
        elif k == 6: s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" * 20); s.settimeout(3); time.sleep(0.02); rst(s)   # pipelined then reset
        elif k == 7: s.sendall(b"\x16\x03\x01garbage\r\n\r\n"); s.settimeout(3); s.recv(100); s.close()
        scen += 1
    except Exception as e:
        print("client error", k, e)
ts = []
for round in range(5):
    ts = [threading.Thread(target=run, args=(i,)) for i in range(400)]
    [t.start() for t in ts]; [t.join() for t in ts]
print("scenarios completed:", scen)
