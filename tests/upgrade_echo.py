"""Helper for tests/smoke.sh: a stand-in backend and a few raw clients.

server PORT [HOST] - a backend for wip to proxy to (HOST defaults to 127.0.0.1; ::1 for IPv6):
                     * "Upgrade: websocket" requests get 101 Switching Protocols, then every
                       byte received is echoed back
                     * GET /both returns a body framed with both Content-Length and chunked
                       encoding, to check how wip relays conflicting framing
                     * anything else returns 200 with the request headers it received as the body
client PORT PATH   - sends an upgrade request to wip at PATH and checks that data
                     comes back through the tunnel unchanged (exit code 0 on success)
slow PORT SECONDS  - starts a request and then sends a header line every half second; exits 0
                     if wip cuts the connection off within SECONDS
slowpost PORT PATH SECONDS - like slow, but trickles a request body to PATH instead
limit PORT N       - opens N idle connections, then checks that one more is refused and that
                     a new connection works again once the N are closed
"""
import socket
import sys
import time


def read_headers(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def server(port, host="127.0.0.1"):
    listener = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, port))
    listener.listen(5)
    while True:
        conn, _ = listener.accept()
        request = read_headers(conn)
        if b"upgrade: websocket" not in request.lower():
            if request.startswith(b"GET /both "):
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n"
                             b"Connection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
            else:
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                             b"Connection: close\r\n\r\n%s" % (len(request), request))
            conn.close()
            continue
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            conn.sendall(chunk)
        conn.close()


def client(port, path):
    conn = socket.create_connection(("127.0.0.1", port), timeout=5)
    conn.sendall((
        "GET %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n" % (path, port)).encode())
    response = read_headers(conn)
    if not response.startswith(b"HTTP/1.1 101"):
        print("expected 101, got: %r" % response[:80])
        sys.exit(1)
    for message in (b"hello", b"x" * 200000):
        conn.sendall(message)
        received = b""
        while len(received) < len(message):
            chunk = conn.recv(65536)
            if not chunk:
                print("tunnel closed early")
                sys.exit(1)
            received += chunk
        if received != message:
            print("echo mismatch")
            sys.exit(1)
    conn.close()


def trickle(conn, piece, limit):
    """Send 'piece' every half second; exit 0 if wip gives up on us within 'limit' seconds."""
    start = time.time()
    try:
        while time.time() - start < limit:
            time.sleep(0.5)
            conn.sendall(piece)
    except OSError:
        sys.exit(0)  # wip closed the connection while we were still sending
    conn.settimeout(1)
    try:
        conn.recv(100)  # A 408 response or end of stream: either way wip gave up on us
        sys.exit(0)
    except socket.timeout:
        sys.exit(1)  # Still open and waiting for more
    except OSError:
        sys.exit(0)


def slow(port, limit):
    conn = socket.create_connection(("127.0.0.1", port), timeout=limit + 5)
    conn.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")
    trickle(conn, b"X-Slow: 1\r\n", limit)


def slowpost(port, path, limit):
    conn = socket.create_connection(("127.0.0.1", port), timeout=limit + 5)
    conn.sendall(b"POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n" % path.encode())
    trickle(conn, b"x" * 10, limit)


def refused(conn):
    conn.settimeout(2)
    try:
        return conn.recv(1) == b""
    except socket.timeout:
        return False
    except OSError:
        return True


def limit(port, n):
    held = [socket.create_connection(("127.0.0.1", port), timeout=5) for _ in range(n)]
    time.sleep(0.3)
    extra = socket.create_connection(("127.0.0.1", port), timeout=5)
    if not refused(extra):
        print("connection %d was not refused" % (n + 1))
        sys.exit(1)
    for conn in held:
        conn.close()
    time.sleep(0.5)
    conn = socket.create_connection(("127.0.0.1", port), timeout=5)
    conn.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    if not conn.recv(100).startswith(b"HTTP/1.1 200"):
        print("no connection accepted after the others closed")
        sys.exit(1)


if __name__ == "__main__":
    if sys.argv[1] == "server":
        server(int(sys.argv[2]), *sys.argv[3:4])
    elif sys.argv[1] == "slow":
        slow(int(sys.argv[2]), float(sys.argv[3]))
    elif sys.argv[1] == "slowpost":
        slowpost(int(sys.argv[2]), sys.argv[3], float(sys.argv[4]))
    elif sys.argv[1] == "limit":
        limit(int(sys.argv[2]), int(sys.argv[3]))
    else:
        client(int(sys.argv[2]), sys.argv[3])
