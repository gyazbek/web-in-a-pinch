# Changelog

## 1.0.0 (2026-11-04)

First public release. Changes since the earlier, unreleased versions:

- Each connection gets its own thread and connections are kept alive, so one browser no longer blocks everyone else.
- Settings can be read from `wip.json`. True/false values work, and a misspelled setting stops startup with the line number instead of being silently ignored. Command-line options override the file.
- The reverse proxy works now (`--proxy /api=http://localhost:3000`, or a `"proxy"` section in the config). It accepts hostnames, streams request and response bodies of any size, sets `X-Forwarded-*` headers, and returns 502/504 when the backend is down or slow.
- The default address is 127.0.0.1. Pass 0.0.0.0 to share on the network.
- Dotfiles, paths that resolve outside the served folder (symlinks included) and the config file are no longer served.
- Security fixes from a review before release:
  - On Windows, junctions and symlinks inside the served folder could point outside it and were followed. Paths are now resolved to their real location on Windows too.
  - Hidden files could be reached through a symlink with an ordinary name or a Windows short name (`GIT~1`). The hidden-file rule now also applies to the real path.
  - The config file is recognised by file identity, so hard links and short names can't expose it.
  - Request smuggling: header names with spaces or other invalid characters (`Content-Length : 5`), bare LF line endings and signed `Content-Length` values are rejected instead of being passed to the backend.
  - Header names with underscores are no longer forwarded by the proxy, so clients can't spoof `X-Forwarded-For` to PHP-style backends.
  - A backend response with both `Content-Length` and chunked encoding is relayed as chunked, without the conflicting `Content-Length`.
  - Request headers (and request bodies sent to static files) must arrive within `keep_alive_timeout` seconds, at most 10, so slow clients can't hold threads indefinitely.
  - Running out of file descriptors no longer stops the server, and a connection is dropped instead of blocking the accept loop when no thread can be created.
  - `max_connections_per_ip` (default 64) limits how many connections one address can hold open.
  - Uploads to proxied routes that slow below 1 KB/s are cut off with a 408.
  - Headers the client lists in its `Connection` header are no longer forwarded to the backend.
- Added HEAD support, URL decoding, and a redirect from `/folder` to `/folder/`.
- Requests with `Expect: 100-continue` get a `100 Continue`, so clients don't stall waiting for one.
- Range requests (single byte ranges, `206`/`416`), so videos can be scrubbed and Safari plays them.
- Folders without an `index.html` get a listing (dotfiles left out). `"enable_directory_listing": false` turns it off.
- The proxy passes WebSocket connections through, so hot reload from dev servers works behind it.
- Each request prints a log line (address, method, path, status, time). `--quiet` or `"enable_request_log": false` turns it off.
- More MIME types: `.mjs`, `.wasm`, `.webp`, `.avif`, fonts, `.mp4`/`.webm`, audio, `.xml`, `.md`, `.csv`, `.map`, `.webmanifest`. Text types are sent with `charset=utf-8`, and `.js` is now `text/javascript`.
- The response cache drops an entry when the file changes, instead of serving it for up to 5 minutes.
- Removed the unused performance code (memory pool, lock-free queue, batching, prefetching, CPU pinning) and the settings that went with it. Old config files still load; those settings print a notice and are ignored.
- The event backend (epoll/kqueue/IOCP) is created when `enable_event_driven` is on, but nothing uses it yet.
- Connection threads use 256 KB stacks instead of the system default (1 MB on Windows, 8 MB on Linux), and large files are sent with `sendfile` on Linux and macOS.
- Build scripts no longer use `-march=native`, so binaries run on other machines. CI runs `tests/smoke.sh` on Linux, macOS and Windows, and release files are named by platform (`wip-linux-x64`, `wip-macos-arm64`, `wip-windows-x64.exe`).
- `tests/fuzz.sh` fuzzes request handling and config loading with libFuzzer and sanitizers; CI runs it on every push.
- `fast_benchmark` measures real elapsed time on every platform (it used CPU time on Linux and macOS), takes an optional path, reads response bodies of any size, reports latency, and works with more than 64 connections on Windows. It replaces `benchmark.ps1`, `benchmark.sh` and `benchmark-high.ps1`.
