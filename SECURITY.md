# Security

wip is meant for local development, not for serving anything on the open internet. It has no HTTPS and no authentication. Still, if you find a way to read files it shouldn't serve, crash it, or smuggle requests through the proxy, I'd like to know.

Please report it privately through GitHub's "Report a vulnerability" button on the Security tab rather than opening a public issue. A description and the request that triggers it is plenty.

What it tries to protect against:

- Serving files outside the chosen folder, whether through `..`, encoded paths, symlinks or (on Windows) junctions.
- Serving dotfiles like `.git` or `.env`, including through a symlink with an ordinary name or a Windows short name such as `GIT~1`.
- Serving its own config file, which is recognised by its file identity rather than its name, so hard links and short names don't get around it.
- Requests that the proxy and the backend behind it could read differently: duplicate or signed `Content-Length`, chunked request bodies, header names that aren't valid tokens (such as `Content-Length : 5`), bare CR or LF line endings, and folded header lines are all rejected. Header names containing underscores are not forwarded, because PHP and similar backends treat `X_Forwarded_For` the same as `X-Forwarded-For`.
- Clients that send a request's headers slowly to hold a connection open: the headers must be complete within `keep_alive_timeout` seconds (at most 10) of the first byte. Uploads to proxied routes must keep arriving at 1 KB/s or more.
- One client taking every connection: `max_connections_per_ip` (64 by default) caps how many connections a single address can have open.
- Running out of file descriptors or threads: the server keeps accepting connections instead of exiting.

It listens on 127.0.0.1 unless you give it another address. If you use 0.0.0.0, anyone on your network can read the folder you're serving, and see its contents in directory listings unless you set `"enable_directory_listing": false`. A slow download only has a timeout between writes, so a client that reads very slowly can keep a thread busy for a long time, but the per-address limit stops one client from doing that to every connection. There's no HTTPS, so anything sent over the network can be read by others on it.
