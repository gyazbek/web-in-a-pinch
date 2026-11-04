#!/usr/bin/env bash
# Fuzz wip with libFuzzer. Needs Linux and clang.
#
# Usage: tests/fuzz.sh [seconds per target]   (default 60)
#
# Builds two fuzzers with AddressSanitizer and UndefinedBehaviorSanitizer:
#   fuzz_request - arbitrary bytes as a client connection (and as the proxy backend's reply)
#   fuzz_config  - arbitrary bytes as a config file
# and runs each for the given time. Inputs that find something new are kept in
# fuzz-out/corpus-*, so later runs pick up where earlier ones stopped. An input that
# crashes is saved in fuzz-out/crashes; reproduce it with
#   fuzz-out/fuzz_request fuzz-out/crashes/request-crash-...

set -eu

TIME="${1:-60}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../fuzz-out"

CC=""
for candidate in clang clang-18 clang-17 clang-16 clang-15 clang-14; do
    if command -v "$candidate" >/dev/null 2>&1; then CC="$candidate"; break; fi
done
if [ -z "$CC" ]; then
    echo "clang not found (on Ubuntu: sudo apt install clang)"
    exit 2
fi

mkdir -p "$OUT/corpus-request" "$OUT/corpus-config" "$OUT/crashes"
FLAGS="-g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -std=c99 -pthread"
echo "Building with $CC"
$CC $FLAGS -o "$OUT/fuzz_request" "$HERE/fuzz_request.c"
$CC $FLAGS -o "$OUT/fuzz_config" "$HERE/fuzz_config.c"

# ---- Starting inputs ----

seed() {  # seed DIR NAME CONTENT(printf format)
    printf "$3" > "$OUT/$1/seed-$2"
}
seed corpus-request get 'GET / HTTP/1.1\r\nHost: x\r\n\r\n'
seed corpus-request head 'HEAD /digits.txt HTTP/1.0\r\nConnection: keep-alive\r\n\r\n'
seed corpus-request range 'GET /big.bin HTTP/1.1\r\nRange: bytes=100-199\r\nIf-Range: x\r\n\r\nGET /digits.txt HTTP/1.1\r\nRange: bytes=-3\r\n\r\n'
seed corpus-request listing 'GET /files/ HTTP/1.1\r\n\r\nGET /files HTTP/1.1\r\n\r\nGET /files/a%%20b.txt HTTP/1.1\r\n\r\n'
seed corpus-request hidden 'GET /.git/config HTTP/1.1\r\n\r\nGET /%%2e%%2e/x HTTP/1.1\r\n\r\nGET /wip.json HTTP/1.1\r\n\r\n'
seed corpus-request stats 'GET /__stats HTTP/1.1\r\n\r\n'
seed corpus-request post 'POST /index.html HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhelloGET / HTTP/1.1\r\nConnection: close\r\n\r\n'
seed corpus-request proxy 'GET /api/x?y=1 HTTP/1.1\r\nHost: h\r\nX-Forwarded-For: 1.2.3.4\r\nConnection: X-A, keep-alive\r\nX-A: 1\r\nX_B: 2\r\n\r\nPOST /keep/y HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc'
seed corpus-request dead 'GET /dead/ HTTP/1.1\r\n\r\n'
seed corpus-request upgrade 'GET /api/ws HTTP/1.1\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\nhello\n--upstream--\nHTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\nworld'
seed corpus-request chunked 'GET /api/c HTTP/1.1\r\n\r\n\n--upstream--\nHTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n'
seed corpus-request close-delimited 'HEAD /api/c HTTP/1.1\r\n\r\nGET /api/d HTTP/1.1\r\n\r\n\n--upstream--\nHTTP/1.0 304 Not Modified\r\nKeep-Alive: 5\r\n\r\nbody'

seed corpus-config full '{\n  "enable_statistics": true,\n  "performance": {\n    "enable_event_driven": false,\n    "batch_size": 4\n  },\n  "proxy": {\n    "/api": "http://localhost:3000",\n    "/v6/*": "http://[::1]:8080/base/"\n  },\n  "max_connections": 10\n}\n'
seed corpus-config bad '{\n  "keep_alive_timeout": "soon",\n  "proxy": { "/x": "https://a" },\n  "nope": 1\n}\n'

cat > "$OUT/http.dict" <<'EOF'
"HTTP/1.1"
"HTTP/1.0"
"\x0d\x0a"
"\x0d\x0a\x0d\x0a"
"GET "
"HEAD "
"POST "
"Content-Length: "
"Transfer-Encoding: chunked"
"Connection: "
"close"
"keep-alive"
"Upgrade"
"Upgrade: websocket"
"Expect: 100-continue"
"Range: bytes="
"If-Range: "
"Host: "
"X-Forwarded-For: "
"%2e"
"%2f"
"%00"
"/api/"
"/keep/"
"/dead/"
"/files/"
"/big.bin"
"/__stats"
"\x0a--upstream--\x0a"
"HTTP/1.1 101 Switching Protocols"
"HTTP/1.1 200 OK"
"\"proxy\": {"
"\"performance\": {"
"\": \""
"http://"
"http://["
"true"
"false"
EOF

# ---- Run ----

status=0
for target in request config; do
    echo
    echo "== fuzz_$target for $TIME seconds"
    rc=0
    "$OUT/fuzz_$target" "$OUT/corpus-$target" -max_total_time="$TIME" -max_len=32768 -timeout=20 \
        -close_fd_mask=3 -print_final_stats=1 -dict="$OUT/http.dict" \
        -artifact_prefix="$OUT/crashes/$target-" > "$OUT/$target.log" 2>&1 || rc=$?
    if [ $rc -eq 0 ]; then
        grep -E "stat::number_of_executed_units|stat::new_units_added" "$OUT/$target.log" || true
        echo "corpus: $(ls "$OUT/corpus-$target" | wc -l) inputs"
    else
        status=1
        tail -60 "$OUT/$target.log"
    fi
done

echo
if [ $status -eq 0 ]; then echo "No crashes"; else echo "Crash found: see fuzz-out/crashes"; fi
exit $status
