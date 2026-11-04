# WIP - Web In a Pinch

A single-file, minimal cross-platform HTTP server written in C that serves static files with proper MIME types. Perfect when you need a web server in a pinch!

It builds on Windows, Linux and macOS and needs nothing beyond the C library and the system's sockets.

## Download

Prebuilt binaries for Linux (x64), macOS (Apple Silicon) and Windows (x64) are on the [Releases](https://github.com/gyazbek/web-in-a-pinch/releases) page. They aren't signed, so:

- Linux: `chmod +x wip-linux-x64` before running it.
- macOS: run `chmod +x wip-macos-arm64` and `xattr -d com.apple.quarantine wip-macos-arm64`, or Gatekeeper will refuse to open it.
- Windows: SmartScreen may warn about an unrecognized app; choose "More info", then "Run anyway". Or build it yourself as below.

## Building

```
make            # Linux/macOS
make windows    # Windows (MinGW)
```

Or call gcc directly:

```
gcc -O2 -Wall -Wextra -std=c99 -pthread -o wip wip.c         # Linux/macOS
gcc -O2 -Wall -Wextra -std=c99 -o wip.exe wip.c -lws2_32      # Windows
```

Tagged releases also get prebuilt binaries from GitHub Actions. To make one, update `WIP_VERSION` in `wip.c` and the CHANGELOG, then push a tag such as `v1.0.0`.

## Running

```
wip [host] [port] [directory] [options]
```

With no arguments it serves the current directory at http://127.0.0.1:8080. Use `0.0.0.0` as the host if other machines on your network should be able to reach it.

```
wip                            # current directory on 127.0.0.1:8080
wip 127.0.0.1 3000 ./public    # ./public on port 3000
wip 0.0.0.0 8080 ./public      # same, but reachable from the network
```

Options:

- `--config FILE` reads settings from FILE instead of `wip.json`
- `--create-config` writes a default `wip.json` and exits (it won't overwrite an existing one)
- `--proxy PREFIX=URL` forwards a path prefix to another server, can be repeated
- `-q`, `--quiet` turns off the per-request log
- `-h`, `--help`

Requesting a folder serves its `index.html`, or a listing of the folder if it doesn't have one. Range requests work, so videos can be scrubbed and downloads resumed. `/__stats` returns request counters as JSON. Connections are kept alive between requests and each connection runs on its own thread.

Each request prints one line with the client address, method, path, status and how long it took:

```
127.0.0.1 GET /app.js 200 0.4 ms
127.0.0.1 GET /api/users 502 1.1 ms
```

## Reverse proxy

Requests under a path prefix can be forwarded to another HTTP server, so a frontend and its API can share one origin:

```
wip --proxy /api=http://localhost:3000
```

`/api/users?id=1` goes to `http://localhost:3000/users?id=1`. If the backend expects to keep the prefix, include it in the target: `--proxy /api=http://localhost:3000/api`. When more than one prefix matches, the longest one wins. Only plain HTTP backends are supported, but the target can be a hostname or an IP address.

Forwarded requests get `X-Forwarded-For`, `X-Forwarded-Host` and `X-Forwarded-Proto` headers. If the backend can't be reached the client gets a 502, and if it doesn't answer within `proxy_timeout` seconds the client gets a 504. Headers that only make sense for one connection aren't forwarded: the standard hop-by-hop ones, anything the client lists in its `Connection` header, and names containing underscores (PHP and similar backends would read `X_Forwarded_For` as `X-Forwarded-For`). Request bodies have to keep arriving at 1 KB/s or more, measured every `keep_alive_timeout` seconds (at most 10); an upload that slows to a trickle gets a 408.

WebSocket connections are passed through too, so a dev server's hot reload (Vite, webpack) keeps working behind the proxy. Once the backend answers with `101 Switching Protocols`, wip just copies bytes in both directions until one side closes.

## Config file

On startup wip reads `wip.json` from the current directory if there is one. Command-line options override it. The repo doesn't include one; `wip --create-config` writes a starting file. The parser is simple and expects one setting per line, the way `--create-config` writes it:

```json
{
  "enable_reverse_proxy": true,
  "enable_response_caching": true,
  "enable_statistics": true,
  "enable_request_log": true,
  "enable_directory_listing": true,

  "performance": {
    "enable_event_driven": false
  },

  "proxy": {
    "/api": "http://localhost:3000"
  },

  "buffer_size": 8192,
  "max_connections": 1000,
  "max_connections_per_ip": 64,
  "keep_alive_timeout": 30,
  "proxy_timeout": 30
}
```

A few notes on the settings:

- The response cache keeps files under 64 KB in memory. An entry is dropped as soon as the file's modification time or size changes, so edits show up on the next reload.
- `buffer_size` is the largest request header block accepted, in bytes.
- `max_connections` caps concurrent connections. Since each one gets a thread, this is also the thread limit.
- `max_connections_per_ip` caps how many of those can come from one address, so a single client can't use them all up. A browser opens about six per site, so 64 leaves plenty of room. Set it to 0 to turn the limit off, or raise it when benchmarking with more connections.
- `keep_alive_timeout` is how many seconds an idle connection stays open. Once a request starts arriving, its headers must be complete within the same number of seconds (at most 10), so a client can't hold a connection by sending them a byte at a time.
- `enable_event_driven` currently only creates the platform's event handle (epoll, kqueue or IOCP). It's groundwork for an event loop; requests are still handled one thread per connection either way.

If a setting is misspelled or has a bad value, wip refuses to start and prints the file and line number.

## What it won't serve

Hidden files and folders (anything starting with a dot, such as `.git` or `.env`), anything that resolves to a path outside the served folder, and its own config file. These checks are made on the real path after following symlinks (and junctions on Windows), so a link with an ordinary name that points at `.git/config` or outside the folder is refused too, as are Windows short names like `GIT~1`. Hidden files are also left out of directory listings. Set `"enable_directory_listing": false` if folders without an `index.html` should return 404 instead.

## Limitations

This is for development and local use. It has no authentication, and no HTTPS (see below). Only single byte ranges are supported; a request for several ranges gets the whole file. Request bodies sent with chunked encoding are rejected; chunked responses from a proxied backend are passed through fine. On Windows, file names with non-ASCII characters may not be served or listed correctly.

## HTTPS

wip only speaks plain HTTP. For local work that's usually fine: browsers treat `http://localhost` and `http://127.0.0.1` as secure, so service workers, `crypto.subtle`, camera and microphone access and the clipboard API all work without HTTPS.

You need HTTPS when you open the site from another device (a phone at `http://192.168.x.x:8080` isn't treated as secure) or when something insists on an `https://` URL. The easiest way is to put a TLS proxy in front of wip:

```
caddy reverse-proxy --from localhost:8443 --to localhost:8080
```

[Caddy](https://caddyserver.com) creates a local certificate authority and installs it on your machine, so `https://localhost:8443` is trusted. [mkcert](https://github.com/FiloSottile/mkcert) does the same if you'd rather use another TLS proxy. To reach it from a phone or share it with someone, a tunnel such as Tailscale Serve, cloudflared or ngrok gives you a real HTTPS URL without any certificate setup.

The proxy also only talks to `http://` backends.

## Tests

`tests/smoke.sh` starts a few wip instances on ports 18080-18085 and checks static files, the security rules, malformed requests, ranges, listings, the proxy (including a WebSocket echo through it), keep-alive and pipelining, connection limits and config file handling with curl. It runs on Linux, macOS and Windows (from Git Bash), and CI runs it on all three:

```
bash tests/smoke.sh ./wip        # or ./wip.exe
```

The WebSocket check uses `tests/upgrade_echo.py` as a stand-in backend and is skipped if Python isn't installed.

`tests/fuzz.sh` fuzzes wip with libFuzzer (Linux and clang only). One fuzzer sends random bytes as a client connection, through request parsing, static files and the proxy (with a fake backend that also sends random replies); the other feeds random config files. Both run with AddressSanitizer and UndefinedBehaviorSanitizer, so memory errors stop the run. CI runs them for two minutes each.

```
bash tests/fuzz.sh 300           # seconds per fuzzer, default 60
```

Interesting inputs are kept in `fuzz-out/`, so each run continues where the last one stopped. Inputs that crash go to `fuzz-out/crashes/`; run the fuzzer with that file as its only argument to reproduce.

## Benchmarking

`fast_benchmark.c` is a small load generator. It opens a number of keep-alive connections, each on its own thread, sends GET requests until it reaches the total, and prints requests per second and latency. It builds the same way on Windows, Linux and macOS:

```
gcc -O2 -pthread -o fast_benchmark fast_benchmark.c          # Linux/macOS
gcc -O2 -o fast_benchmark.exe fast_benchmark.c -lws2_32       # Windows

./fast_benchmark <host> <port> <total_requests> <connections> [path]
./fast_benchmark 127.0.0.1 8080 100000 50                # hits /__stats
./fast_benchmark 127.0.0.1 8080 10000 10 /index.html
```

It exits with status 1 if any request failed, and prints the status code it got, so it doubles as a quick check that a path works under load. Start the server with `--quiet` first, otherwise printing the log becomes the bottleneck, and set `max_connections_per_ip` to 0 if you use more than 64 connections. Over loopback with 50 connections I see about 530k requests per second for a small static file under WSL and 13–18k on Windows (it varies between runs), where resolving the real path of every requested file (to catch junctions and short names) is much slower. `/__stats`, which doesn't touch the disk, does about 195k on Windows. On Linux and macOS large files are sent with `sendfile`, so the kernel copies them straight to the socket. Either way the server won't be the bottleneck for local work.

## License

MIT, see [LICENSE](LICENSE).
