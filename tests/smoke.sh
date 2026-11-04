#!/usr/bin/env bash
# Smoke tests for wip. Runs on Linux, macOS and Windows (Git Bash).
#
# Usage: tests/smoke.sh path/to/wip
#
# Starts wip instances on ports 18080-18084 (a front server, a proxy upstream and a few
# short-lived ones with special configs), runs requests against them with curl and prints
# PASS/FAIL lines. Exits non-zero if anything failed.

set -u

if [ $# -ne 1 ]; then
    echo "Usage: $0 path/to/wip"
    exit 2
fi

WIP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
FRONT=18080
UP=18081
WS=18082
OTHER=18083
LIMITED=18084
V6=18085
B="http://127.0.0.1:$FRONT"
fails=0
pids=()

cleanup() {
    if [ ${#pids[@]} -gt 0 ]; then
        kill "${pids[@]}" 2>/dev/null
        wait "${pids[@]}" 2>/dev/null  # Without this bash prints a "Terminated" line for each
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
    *) WINDOWS=0 ;;
esac

# Git Bash rewrites arguments that look like Unix paths (e.g. "--proxy /api=...") when
# calling Windows programs. Turn that off and convert real paths explicitly instead.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
native() {
    if [ $WINDOWS -eq 1 ]; then cygpath -w "$1"; else echo "$1"; fi
}
# Where curl should throw away output (with path conversion off, Windows curl needs NUL)
if [ $WINDOWS -eq 1 ]; then DISCARD=NUL; else DISCARD=/dev/null; fi

PY=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "import socket" >/dev/null 2>&1; then
        PY="$candidate"
        break
    fi
done

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); failed="$failed  $1
"; }
failed=""

# expect_status NAME CODE CURL-ARGS...
expect_status() {
    local name="$1" want="$2"
    shift 2
    local got
    got=$(curl -s -o "$DISCARD" -w '%{http_code}' --path-as-is "$@")
    if [ "$got" = "$want" ]; then pass "$name ($got)"; else fail "$name (got $got, want $want)"; fi
}

# expect_body NAME EXPECTED CURL-ARGS...   (exact body match)
expect_body() {
    local name="$1" want="$2"
    shift 2
    local got
    got=$(curl -s --path-as-is "$@")
    if [ "$got" = "$want" ]; then pass "$name"; else fail "$name (got '$got', want '$want')"; fi
}

wait_for() {
    for _ in $(seq 1 50); do
        curl -s -o "$DISCARD" "$1" && return 0
        sleep 0.1
    done
    return 1
}

# raw_status NAME CODE REQUEST   (REQUEST is a printf format, so \r\n can be spelled out)
raw_status() {
    local name="$1" want="$2" request="$3" line got
    if ! exec 3<>"/dev/tcp/127.0.0.1/$FRONT"; then
        fail "$name (cannot connect)"
        return
    fi
    printf "$request" >&3
    IFS= read -r -t 5 line <&3
    exec 3<&- 3>&-
    got=$(echo "$line" | cut -d' ' -f2)
    if [ "$got" = "$want" ]; then pass "$name ($got)"; else fail "$name (got '$got', want $want)"; fi
}

# raw_responses NAME EXPECTED REQUEST
# Sends REQUEST on one connection and lists the status of every response that comes back,
# followed by "closed" if the server then closed the connection or "open" if it kept it
# open, e.g. "200 200 closed".
raw_responses() {
    local name="$1" want="$2" request="$3" line rc got="" end
    if ! exec 3<>"/dev/tcp/127.0.0.1/$FRONT"; then
        fail "$name (cannot connect)"
        return
    fi
    printf "$request" >&3
    while :; do
        local started=$SECONDS
        IFS= read -r -t 2 line <&3
        rc=$?
        # bash 4+ returns >128 on a timeout; bash 3.2 (macOS) returns 1 as for end of
        # stream, so there a failed read that took the full 2 seconds counts as a timeout
        if [ $rc -gt 128 ] || { [ $rc -ne 0 ] && [ -z "$line" ] && [ $((SECONDS - started)) -ge 2 ]; }; then
            end=open
            break
        fi
        case "$line" in HTTP/1.*) got="$got$(echo "$line" | cut -d' ' -f2) " ;; esac
        if [ $rc -ne 0 ]; then end=closed; break; fi
    done
    exec 3<&- 3>&-
    got="$got$end"
    if [ "$got" = "$want" ]; then pass "$name"; else fail "$name (got '$got', want '$want')"; fi
}

# run_briefly CMD...   Runs a command that is expected to exit on its own and returns its
# exit code, or 124 if it was still running after 5 seconds. Output goes to $WORK/brief.log.
run_briefly() {
    "$@" > "$WORK/brief.log" 2>&1 &
    local pid=$!
    for _ in $(seq 1 50); do
        kill -0 $pid 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 $pid 2>/dev/null; then
        kill $pid
        wait $pid 2>/dev/null
        return 124
    fi
    wait $pid
}

# ---- Test files ----

mkdir -p "$WORK/site/docs" "$WORK/site/files/sub" "$WORK/site/.git" "$WORK/up/sub" "$WORK/outside"
echo '<h1>site</h1>' > "$WORK/site/index.html"
echo '<h1>docs</h1>' > "$WORK/site/docs/index.html"
echo 'secret' > "$WORK/site/.git/config"
echo 'spaces ok' > "$WORK/site/my file.txt"
printf '0123456789' > "$WORK/site/digits.txt"
echo 'listed' > "$WORK/site/files/a.txt"
echo 'hidden' > "$WORK/site/files/.hidden"
echo 'from upstream' > "$WORK/up/hello.txt"
echo 'from upstream/sub' > "$WORK/up/sub/hello.txt"
head -c 300000 /dev/urandom > "$WORK/up/big.bin"
echo 'outside' > "$WORK/outside/secret.txt"
if [ $WINDOWS -eq 0 ]; then
    ln -s "$WORK/outside/secret.txt" "$WORK/site/escape.txt"
    ln -s "$WORK/site/.git/config" "$WORK/site/gitconfig.txt"
else
    cmd /c mklink /J "$(native "$WORK/site/junction")" "$(native "$WORK/outside")" > /dev/null
fi
cat > "$WORK/site/wip.json" <<EOF
{
  "enable_request_log": false,
  "proxy": {
    "/cfg": "http://localhost:$UP"
  },
  "keep_alive_timeout": 3,
  "max_connections_per_ip": 8,
  "proxy_timeout": 5
}
EOF

# ---- Servers ----

"$WIP" 127.0.0.1 $UP "$(native "$WORK/up")" --config "$(native "$WORK/none.json")" --quiet > "$WORK/up.log" 2>&1 &
pids+=($!)
"$WIP" 127.0.0.1 $FRONT "$(native "$WORK/site")" --config "$(native "$WORK/site/wip.json")" \
    --proxy /api=http://localhost:$UP \
    --proxy "/keep/*=http://127.0.0.1:$UP/sub" \
    --proxy /dead=http://127.0.0.1:9 \
    --proxy /ws=http://127.0.0.1:$WS \
    --proxy /echo=http://127.0.0.1:$WS \
    --proxy "/v6=http://[::1]:$V6" > "$WORK/front.log" 2>&1 &
pids+=($!)
HAVE_V6=0
if [ -n "$PY" ]; then
    "$PY" "$(native "$HERE/upgrade_echo.py")" server $WS > "$WORK/ws.log" 2>&1 &
    pids+=($!)
    if "$PY" -c "import socket; socket.socket(socket.AF_INET6).bind(('::1', 0))" 2>/dev/null; then
        "$PY" "$(native "$HERE/upgrade_echo.py")" server $V6 ::1 > "$WORK/v6.log" 2>&1 &
        pids+=($!)
        HAVE_V6=1
    fi
fi

if ! wait_for "$B/" || ! wait_for "http://127.0.0.1:$UP/hello.txt"; then
    echo "Servers did not start. Front log:"
    cat "$WORK/front.log"
    exit 1
fi

# ---- Static files ----

expect_status "index" 200 "$B/"
expect_status "HEAD" 200 -I "$B/"
expect_status "folder without slash redirects" 301 "$B/docs"
expect_status "folder index" 200 "$B/docs/"
expect_status "encoded space" 200 "$B/my%20file.txt"
expect_status "missing file" 404 "$B/nope"
expect_status "POST to a static file" 405 -X POST -d x "$B/"
expect_status "stats" 200 "$B/__stats"

# ---- Things that must not be served ----

expect_status "dotfile" 403 "$B/.git/config"
expect_status "traversal" 403 "$B/../outside/secret.txt"
expect_status "encoded traversal" 403 "$B/%2e%2e/outside/secret.txt"
expect_status "encoded NUL" 400 "$B/index.html%00"
expect_status "config file" 404 "$B/wip.json"
if [ $WINDOWS -eq 0 ]; then
    expect_status "symlink leaving the folder" 403 "$B/escape.txt"
    expect_status "symlink to a hidden file" 403 "$B/gitconfig.txt"
else
    expect_status "junction leaving the folder" 403 "$B/junction/secret.txt"
    short=$(cd "$WORK/site" && cmd /c "for %I in (.git) do @echo %~snxI" | tr -d '\r')
    if [ -n "$short" ] && [ "$short" != ".git" ]; then
        expect_status "hidden folder by its short name ($short)" 403 "$B/$short/config"
    else
        echo "SKIP hidden folder by short name (8.3 names are off on this drive)"
    fi
fi

# ---- Malformed and ambiguous requests ----

raw_status "space before the colon in a header name" 400 'GET / HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\n\r\n'
raw_status "bare LF line ending" 400 'GET / HTTP/1.1\nHost: x\r\n\r\n'
raw_status "Content-Length with a sign" 400 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\nhello'
raw_status "two Content-Length headers" 400 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!'
raw_status "chunked request body" 501 'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n'
raw_status "well-formed raw request" 200 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
raw_status "URL too long" 414 "GET /$(head -c 1100 /dev/zero | tr '\0' a) HTTP/1.1\r\nHost: x\r\n\r\n"
# Exactly fills the 8 KB request buffer without finishing the headers
raw_status "headers too large" 431 "GET / HTTP/1.1\r\nHost: x\r\nX-Big: $(head -c 8160 /dev/zero | tr '\0' a)"
raw_status "HTTP/2 request line" 505 'GET / HTTP/2.0\r\nHost: x\r\n\r\n'
raw_status "absolute-form target" 400 'GET http://example.com/ HTTP/1.1\r\nHost: x\r\n\r\n'

# ---- Keep-alive and pipelining ----

raw_responses "HTTP/1.1 stays open" "200 open" 'GET / HTTP/1.1\r\nHost: x\r\n\r\n'
raw_responses "HTTP/1.0 closes by default" "200 closed" 'GET / HTTP/1.0\r\n\r\n'
raw_responses "HTTP/1.0 keep-alive on request" "200 200 closed" \
    'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /digits.txt HTTP/1.0\r\n\r\n'
raw_responses "pipelined requests" "200 200 closed" \
    'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /digits.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
raw_responses "pipelined request after a body" "405 200 closed" \
    'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
hundred=""
requests=""
for _ in $(seq 1 100); do
    hundred="${hundred}200 "
    requests="${requests}GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"
done
raw_responses "connection closed after 100 requests" "${hundred}closed" "$requests"
raw_status "Expect: 100-continue on a static file" 100 \
    'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n'
raw_status "Expect: 100-continue through the proxy" 100 \
    'POST /api/hello.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n'

# ---- Range requests ----

expect_status "range" 206 -H "Range: bytes=2-4" "$B/digits.txt"
expect_body "range body" "234" -H "Range: bytes=2-4" "$B/digits.txt"
expect_body "suffix range" "789" -H "Range: bytes=-3" "$B/digits.txt"
expect_body "open-ended range" "89" -H "Range: bytes=8-" "$B/digits.txt"
expect_status "unsatisfiable range" 416 -H "Range: bytes=20-" "$B/digits.txt"
expect_status "multiple ranges fall back to the whole file" 200 -H "Range: bytes=0-1,3-4" "$B/digits.txt"

# ---- Directory listing ----

listing=$(curl -s "$B/files/")
if [[ "$listing" == *'href="a.txt"'* && "$listing" == *'href="sub/"'* ]]; then pass "listing shows files and folders"; else fail "listing shows files and folders"; fi
if [[ "$listing" != *".hidden"* ]]; then pass "listing hides dotfiles"; else fail "listing hides dotfiles"; fi

# ---- Reverse proxy ----

expect_body "proxy by hostname" "from upstream" "$B/api/hello.txt"
expect_body "proxy route from config file" "from upstream" "$B/cfg/hello.txt"
expect_body "proxy base path" "from upstream/sub" "$B/keep/hello.txt"
expect_status "prefix needs a segment boundary" 404 "$B/apix"
expect_status "backend down" 502 "$B/dead/x"
expect_status "backend's own status is relayed" 405 -X POST -d hello=1 "$B/api/hello.txt"
expect_status "range through the proxy" 206 -H "Range: bytes=0-9" "$B/api/big.bin"
curl -s -o "$(native "$WORK/big.out")" "$B/api/big.bin"
if cmp -s "$WORK/up/big.bin" "$WORK/big.out"; then pass "300 KB file through the proxy is identical"; else fail "300 KB file through the proxy is identical"; fi

if [ -n "$PY" ]; then
    if "$PY" "$(native "$HERE/upgrade_echo.py")" client $FRONT /ws/chat; then pass "WebSocket upgrade through the proxy"; else fail "WebSocket upgrade through the proxy"; fi

    # The backend echoes the headers it received
    seen=$(curl -s -H "X_Forwarded_For: 6.6.6.6" -H "X-Forwarded-For: 10.0.0.1" "$B/echo/headers")
    if [[ "$seen" != *"6.6.6.6"* ]]; then pass "underscore header variant not forwarded"; else fail "underscore header variant not forwarded"; fi
    if [[ "$seen" == *"X-Forwarded-For: 10.0.0.1, 127.0.0.1"* ]]; then pass "X-Forwarded-For appends the client address"; else fail "X-Forwarded-For appends the client address"; fi

    # Backend sends both Content-Length and chunked: chunked wins and Content-Length is dropped
    expect_body "conflicting framing from the backend" "hello" "$B/echo/both"
    if ! curl -s -D - -o "$DISCARD" "$B/echo/both" | grep -qi "^Content-Length"; then pass "conflicting Content-Length not passed on"; else fail "conflicting Content-Length not passed on"; fi

    # A client that trickles header lines is cut off (keep_alive_timeout is 3 s in this config)
    if "$PY" "$(native "$HERE/upgrade_echo.py")" slow $FRONT 6; then pass "slow client cut off"; else fail "slow client cut off"; fi

    # Headers named in the Connection header are for this hop only and aren't forwarded
    seen=$(curl -s -H "Connection: X-Secret" -H "X-Secret: 1" "$B/echo/headers")
    if [[ "$seen" != *"X-Secret"* ]]; then pass "headers listed in Connection not forwarded"; else fail "headers listed in Connection not forwarded"; fi

    # An upload that slows to a trickle is cut off; a normal large one still goes through
    if "$PY" "$(native "$HERE/upgrade_echo.py")" slowpost $FRONT /api/upload 8; then pass "trickling upload cut off"; else fail "trickling upload cut off"; fi
    expect_status "300 KB upload through the proxy" 405 -X POST --data-binary "@$(native "$WORK/up/big.bin")" "$B/api/upload"
else
    echo "SKIP WebSocket, header forwarding and slow-client checks (no python found)"
fi

if [ $HAVE_V6 -eq 1 ]; then
    seen=$(curl -s "$B/v6/x")
    if [[ "$seen" == *"GET /x HTTP/1.1"* && "$seen" == *"Host: [::1]:$V6"* ]]; then pass "IPv6 backend"; else fail "IPv6 backend"; fi
else
    echo "SKIP IPv6 backend (no python or no IPv6 loopback)"
fi

# ---- Connections and caching ----

reused=$(curl -s -v -o "$DISCARD" -o "$DISCARD" -o "$DISCARD" "$B/" "$B/api/hello.txt" "$B/" 2>&1 | grep -Ec "Re-?using existing")
if [ "$reused" = "2" ]; then pass "keep-alive reuses the connection"; else fail "keep-alive reuses the connection (reused $reused times, want 2)"; fi

if curl -s -D - -o "$DISCARD" -H "connection: CLOSE" "$B/" | grep -qi "^Connection: close"; then pass "lowercase 'connection: CLOSE' is honored"; else fail "lowercase 'connection: CLOSE' is honored"; fi

curl -s -o "$DISCARD" "$B/"
echo '<h1>site, edited</h1>' > "$WORK/site/index.html"
if [[ "$(curl -s "$B/")" == *edited* ]]; then pass "edited file is served fresh"; else fail "edited file is served fresh"; fi

curl -s --max-time 0.01 -o "$DISCARD" "$B/api/big.bin"
sleep 0.3
expect_status "server survives a client disconnecting mid-download" 200 "$B/"

if ! grep -q " GET " "$WORK/front.log"; then pass "request log turned off by the config file"; else fail "request log turned off by the config file"; fi

# max_connections_per_ip is 8 in this config
if [ -n "$PY" ]; then
    sleep 0.5
    if "$PY" "$(native "$HERE/upgrade_echo.py")" limit $FRONT 8; then pass "per-IP connection limit"; else fail "per-IP connection limit"; fi
fi

# ---- Config files and other server instances ----

printf '{\n  "keep_alive_timeout": "soon",\n  "enable_statistics": maybe\n}\n' > "$WORK/bad.json"
run_briefly "$WIP" 127.0.0.1 $OTHER "$(native "$WORK/site")" --config "$(native "$WORK/bad.json")"
rc=$?
if [ $rc -eq 1 ] && grep -q "bad.json:2" "$WORK/brief.log" && grep -q "bad.json:3" "$WORK/brief.log"; then
    pass "invalid config file is reported and refused"
else
    fail "invalid config file is reported and refused (exit $rc)"
fi

run_briefly "$WIP" --config "$(native "$WORK/new.json")" --create-config
rc=$?
if [ $rc -eq 0 ] && [ -f "$WORK/new.json" ]; then pass "--create-config writes a file"; else fail "--create-config writes a file (exit $rc)"; fi
echo '{}' > "$WORK/keep.json"
run_briefly "$WIP" --config "$(native "$WORK/keep.json")" --create-config
rc=$?
if [ $rc -eq 1 ] && [ "$(cat "$WORK/keep.json")" = "{}" ]; then pass "--create-config won't overwrite"; else fail "--create-config won't overwrite (exit $rc)"; fi

# The generated config file must load and work
"$WIP" 127.0.0.1 $OTHER "$(native "$WORK/site")" --config "$(native "$WORK/new.json")" > "$WORK/other.log" 2>&1 &
other=$!
pids+=($other)
if wait_for "http://127.0.0.1:$OTHER/"; then pass "server runs with the generated config"; else fail "server runs with the generated config"; fi
kill $other 2>/dev/null

# max_connections caps connections from everyone combined
printf '{\n  "max_connections": 2,\n  "max_connections_per_ip": 0,\n  "enable_request_log": false\n}\n' > "$WORK/limited.json"
"$WIP" 127.0.0.1 $LIMITED "$(native "$WORK/site")" --config "$(native "$WORK/limited.json")" > "$WORK/limited.log" 2>&1 &
pids+=($!)
if [ -n "$PY" ] && wait_for "http://127.0.0.1:$LIMITED/"; then
    sleep 0.3
    if "$PY" "$(native "$HERE/upgrade_echo.py")" limit $LIMITED 2; then pass "max_connections"; else fail "max_connections"; fi
fi

echo
echo "$fails failure(s)"
if [ $fails -gt 0 ]; then printf '%s' "$failed"; fi
[ $fails -eq 0 ]
