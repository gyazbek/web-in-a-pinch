/*
 * WIP - Web In a Pinch
 * A small single-file HTTP server with an optional reverse proxy
 * 
 * Serves static files with proper MIME types and can forward requests under
 * chosen path prefixes to other HTTP servers.
 * Perfect when you need a web server (and proxy) in a pinch!
 * 
 * Usage: wip [host] [port] [directory] [--proxy /prefix=http://host:port[/path]]
 * 
 * Features:
 * - Single C source file (no dependencies beyond the C library and sockets)
 * - Cross-platform (Windows, Linux, macOS)
 * - Static file serving with MIME types, HEAD support and keep-alive
 * - One thread per connection
 * - In-memory cache for small files (refreshed when a file changes)
 * - Reverse proxy by path prefix (HTTP upstreams)
 * - Runtime configuration from a JSON file (wip.json)
 */

// ========================================
// PLATFORM SETUP & INCLUDES
// ========================================

// Windows compatibility - must be defined before any Windows headers
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600  // Windows Vista and later for inet_pton/inet_ntop
    #endif
#endif

#define _GNU_SOURCE    // Enable GNU extensions
#define WIP_VERSION "1.0.0"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <signal.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <process.h>
    #define close closesocket
    #define strcasecmp _stricmp
    #define strncasecmp _strnicmp
    typedef int socklen_t;
    #ifndef _SSIZE_T_DEFINED
    typedef int ssize_t;
    #endif
#else
    #include <unistd.h>
    #include <strings.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <pthread.h>
    #include <dirent.h>
    #if defined(__linux__)
        #include <sys/sendfile.h>
    #elif defined(__APPLE__)
        #include <sys/types.h>
        #include <sys/uio.h>
    #endif
#endif

#ifndef S_ISREG
    #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

// ========================================
// CONFIGURATION
// ========================================

#define MAX_PATH_LEN 1024                // Maximum URL path / file path length
#define MAX_REQUESTS_PER_CONNECTION 100  // Close keep-alive connections after this many requests
#define MAX_PROXY_TARGETS 16             // Maximum reverse proxy routes
#define PROXY_HEADER_BUFFER 16384        // Maximum size of an upstream response header block
#define IO_CHUNK_SIZE 16384              // Chunk size for file and proxy body transfers
#define CACHE_MAX_FILE_SIZE (64 * 1024)  // Only cache files smaller than this
#define MAX_DISCARDED_BODY (1024 * 1024) // Largest unused request body we read to keep a connection open
#define THREAD_STACK_SIZE (256 * 1024)   // Per-connection thread stack; the deepest path uses well under 64 KB
#define MIN_UPLOAD_RATE 1024             // Bytes per second a proxied request body must keep up

// Runtime configuration structure (loaded from wip.json)
typedef struct {
    int enable_reverse_proxy;
    int enable_response_caching;
    int enable_statistics;
    int enable_event_driven;     // Experimental: only initializes the event backend for now
    int enable_request_log;      // Print one line per request
    int enable_directory_listing; // List folders that have no index.html
    int buffer_size;             // Maximum request header size in bytes
    int max_connections;         // Maximum concurrent connections (one thread each)
    int max_connections_per_ip;  // Maximum concurrent connections from one client address (0 = no limit)
    int keep_alive_timeout;      // Seconds an idle keep-alive connection stays open
    int proxy_timeout;           // Seconds to wait on an upstream server
    char config_file[256];
} wip_config_t;

// Global runtime configuration (defaults, overridden by wip.json and the command line)
static wip_config_t config = {
    .enable_reverse_proxy = 1,
    .enable_response_caching = 1,
    .enable_statistics = 1,
    .enable_event_driven = 0,
    .enable_request_log = 1,
    .enable_directory_listing = 1,
    .buffer_size = 8192,
    .max_connections = 1000,
    .max_connections_per_ip = 64,
    .keep_alive_timeout = 30,
    .proxy_timeout = 30,
    .config_file = "wip.json"
};

#define BUFFER_SIZE config.buffer_size
#define MAX_CONNECTIONS config.max_connections
#define KEEP_ALIVE_TIMEOUT config.keep_alive_timeout

// ========================================
// GLOBAL STATE
// ========================================

static int server_socket = -1;
static char server_root[MAX_PATH_LEN];       // Absolute path of the served directory
static time_t server_start_time;

// Statistics (connections are handled on multiple threads, so update atomically)
static volatile long total_requests = 0;
static volatile long successful_requests = 0;
static volatile long proxy_requests = 0;
static volatile long proxy_errors = 0;
static volatile long active_connection_threads = 0;

#ifdef _WIN32
    #define ATOMIC_INC(x) InterlockedIncrement(&(x))
    #define ATOMIC_DEC(x) InterlockedDecrement(&(x))
#else
    #define ATOMIC_INC(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)
    #define ATOMIC_DEC(x) __atomic_sub_fetch(&(x), 1, __ATOMIC_RELAXED)
#endif

// ========================================
// SOCKET & PATH UTILITIES
// ========================================

// Send the whole buffer, handling partial writes. Returns 0 on success, -1 on error.
static int send_all(int sock, const char* data, size_t len) {
    while (len > 0) {
        int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
        ssize_t sent = send(sock, data, chunk, 0);
        if (sent <= 0) {
#ifndef _WIN32
            if (sent < 0 && errno == EINTR) continue;
#endif
            return -1;
        }
        data += sent;
        len -= (size_t)sent;
    }
    return 0;
}

// True if the last socket error was a send/receive timeout
static int socket_timed_out(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Apply receive and send timeouts (in seconds) to a socket
static void set_socket_timeouts(int sock, int seconds) {
#ifdef _WIN32
    DWORD timeout_ms = (DWORD)seconds * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static void set_socket_blocking(int sock, int blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags != -1) {
        fcntl(sock, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
    }
#endif
}

// Resolve a path to an absolute, normalized path without a trailing separator.
// On POSIX this also resolves symlinks and fails if the path does not exist.
// Returns 0 on success, -1 on failure.
static int resolve_path(const char* path, char* out, size_t out_size) {
#ifdef _WIN32
    char* resolved = _fullpath(NULL, path, 0);
#else
    char* resolved = realpath(path, NULL);
#endif
    if (!resolved) return -1;
    size_t len = strlen(resolved);
    if (len >= out_size) {
        free(resolved);
        return -1;
    }
    memcpy(out, resolved, len + 1);
    free(resolved);
    // Drop trailing separators (Windows stat() rejects them) but keep "/" and "C:\"
    while (len > 1 && (out[len - 1] == '/' || out[len - 1] == '\\') && !(len == 3 && out[1] == ':')) {
        out[--len] = '\0';
    }
    return 0;
}

// True if 'path' is 'root' itself or somewhere inside it
static int path_is_within(const char* path, const char* root) {
    size_t root_len = strlen(root);
#ifdef _WIN32
    if (_strnicmp(path, root, root_len) != 0) return 0;
#else
    if (strncmp(path, root, root_len) != 0) return 0;
#endif
    if (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) return 1;
    return path[root_len] == '\0' || path[root_len] == '/' || path[root_len] == '\\';
}

// Seconds a client gets to send a complete header block (or an unused body) once it has
// started; without a total limit, a client sending a byte every few seconds holds a thread forever
static int request_deadline_seconds(void) {
    return KEEP_ALIVE_TIMEOUT < 10 ? KEEP_ALIVE_TIMEOUT : 10;
}

// Resolve 'path' to the real absolute path of an existing file or folder, following
// symlinks (and junctions on Windows), and fill in its type, size and modification time.
// If 'id' is not NULL it receives a value that identifies the file itself (volume and
// file number, or device and inode), which also matches through hard links and short names.
// Returns 0 on success, -1 if it doesn't exist or can't be resolved.
typedef struct {
    uint64_t volume;
    uint64_t index;
} file_id_t;

static file_id_t config_id;      // The config file itself, which is never served
static int have_config_id = 0;   // Whether the config file exists (and config_id is set)

static int resolve_existing(const char* path, char* out, size_t out_size, struct stat* st, file_id_t* id) {
#ifdef _WIN32
    // Normalize the string first. Device names such as "COM1" come out as "\\.\COM1"
    // and are rejected here, before anything is opened.
    char normalized[MAX_PATH_LEN];
    if (resolve_path(path, normalized, sizeof(normalized)) < 0) return -1;
    int drive_path = normalized[0] != '\0' && normalized[1] == ':';
    int unc_path = normalized[0] == '\\' && normalized[1] == '\\' && normalized[2] != '.' && normalized[2] != '?';
    if (!drive_path && !unc_path) return -1;

    // Open just for attributes; the handle tells us where the path really ends up
    HANDLE handle = CreateFileA(normalized, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    char final_path[MAX_PATH_LEN + 8];
    BY_HANDLE_FILE_INFORMATION info;
    // Paths with '~' may contain 8.3 short names ("GIT~1" for ".git"), so ask for the full
    // long-name form for those. It's slower, which is why other paths skip it.
    DWORD name_form = strchr(normalized, '~') ? FILE_NAME_NORMALIZED : FILE_NAME_OPENED;
    DWORD final_len = GetFinalPathNameByHandleA(handle, final_path, sizeof(final_path),
                                                name_form | VOLUME_NAME_DOS);
    int ok = final_len > 0 && final_len < sizeof(final_path) && GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!ok) return -1;
    if (info.nFileSizeHigh != 0 || info.nFileSizeLow > (DWORD)LONG_MAX) return -1;  // Over 2 GB

    // Drop the "\\?\" prefix; "\\?\UNC\server\share" becomes "\\server\share"
    const char* real_path = final_path;
    char unc[MAX_PATH_LEN + 8];
    if (strncmp(final_path, "\\\\?\\UNC\\", 8) == 0) {
        snprintf(unc, sizeof(unc), "\\\\%s", final_path + 8);
        real_path = unc;
    } else if (strncmp(final_path, "\\\\?\\", 4) == 0) {
        real_path = final_path + 4;
    }
    size_t len = strlen(real_path);
    if (len >= out_size) return -1;
    memcpy(out, real_path, len + 1);
    while (len > 1 && out[len - 1] == '\\' && !(len == 3 && out[1] == ':')) out[--len] = '\0';

    memset(st, 0, sizeof(*st));
    st->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG;
    st->st_size = (long)info.nFileSizeLow;
    uint64_t write_time = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
    st->st_mtime = (time_t)((write_time - 116444736000000000ULL) / 10000000ULL);  // 100 ns since 1601 -> Unix time
    if (id) {
        id->volume = info.dwVolumeSerialNumber;
        id->index = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    }
    return 0;
#else
    if (resolve_path(path, out, out_size) < 0) return -1;
    if (stat(out, st) != 0) return -1;
    if (id) {
        id->volume = (uint64_t)st->st_dev;
        id->index = (uint64_t)st->st_ino;
    }
    return 0;
#endif
}

// Milliseconds from a monotonic clock, used to time requests for the log
static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

// ========================================
// HTTP PARSING
// ========================================

typedef struct {
    char method[16];
    char target[MAX_PATH_LEN];  // Request target as sent: path plus optional query string
    char version[16];
    const char* headers;        // Header lines (points into the connection buffer)
    size_t headers_len;
    long content_length;
    int has_content_length;
    int has_transfer_encoding;
    int keep_alive;             // Client allows the connection to stay open
    int expect_continue;        // Client sent "Expect: 100-continue"
    char upgrade[64];           // Protocol requested with "Connection: Upgrade" (e.g. "websocket"), or ""
    char connection[256];       // Value of the Connection header, or ""
} http_request_t;

// Find the end of the header block. Returns the offset just past "\r\n\r\n", or 0.
static size_t find_header_end(const char* buf, size_t len) {
    const char* p = buf;
    const char* end = buf + len;
    while (p < end && (p = memchr(p, '\n', (size_t)(end - p))) != NULL) {
        if (p - buf >= 3 && p[-1] == '\r' && p[-2] == '\n' && p[-3] == '\r') {
            return (size_t)(p - buf) + 1;
        }
        p++;
    }
    return 0;
}

// Iterate over "Name: value" header lines. Returns 1 with trimmed name/value spans
// for each well-formed line, 0 when there are no more lines.
static int next_header(const char** cursor, const char* end,
                       const char** name, size_t* name_len,
                       const char** value, size_t* value_len) {
    while (*cursor < end) {
        const char* line = *cursor;
        const char* eol = memchr(line, '\n', (size_t)(end - line));
        const char* line_end = eol ? eol : end;
        *cursor = eol ? eol + 1 : end;
        if (line_end > line && line_end[-1] == '\r') line_end--;

        const char* colon = memchr(line, ':', (size_t)(line_end - line));
        if (!colon || colon == line) continue;  // Skip malformed and empty lines

        const char* v = colon + 1;
        while (v < line_end && (*v == ' ' || *v == '\t')) v++;
        const char* v_end = line_end;
        while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) v_end--;

        *name = line;
        *name_len = (size_t)(colon - line);
        *value = v;
        *value_len = (size_t)(v_end - v);
        return 1;
    }
    return 0;
}

static int header_name_is(const char* name, size_t name_len, const char* expected) {
    return strlen(expected) == name_len && strncasecmp(name, expected, name_len) == 0;
}

// Copy the value of a header (name matched case-insensitively). Returns 1 if found.
static int find_header(const char* headers, size_t len, const char* wanted, char* out, size_t out_size) {
    const char* cursor = headers;
    const char *name, *value;
    size_t name_len, value_len;
    while (next_header(&cursor, headers + len, &name, &name_len, &value, &value_len)) {
        if (header_name_is(name, name_len, wanted)) {
            if (value_len >= out_size) value_len = out_size - 1;
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return 1;
        }
    }
    return 0;
}

// True if a comma-separated header value contains the token (case-insensitive)
static int header_has_token(const char* value, const char* token) {
    size_t token_len = strlen(token);
    const char* p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - start) == token_len && strncasecmp(start, token, token_len) == 0) return 1;
    }
    return 0;
}

// Characters allowed in a header name (an HTTP "token")
static int is_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

// Parse the request line and headers. memchr (vectorized by the C library) locates
// the delimiters. Returns 0 on success, or an HTTP status code to reply with.
static int parse_request(const char* buf, size_t header_len, http_request_t* req) {
    memset(req, 0, sizeof(*req));

    // Reject NUL bytes, bare CR or LF and folded header lines: they are ambiguous and
    // could be used to smuggle headers past the proxy
    for (size_t i = 0; i < header_len; i++) {
        char c = buf[i];
        if (c == '\0') return 400;
        if (c == '\r' && (i + 1 >= header_len || buf[i + 1] != '\n')) return 400;
        if (c == '\n' && (i == 0 || buf[i - 1] != '\r')) return 400;
        if (c == '\n' && i + 1 < header_len && (buf[i + 1] == ' ' || buf[i + 1] == '\t')) return 400;
    }

    // Request line: METHOD SP TARGET SP VERSION CRLF
    const char* line_end = memchr(buf, '\r', header_len);
    if (!line_end) return 400;
    const char* sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (!sp1) return 400;
    const char* sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2) return 400;

    size_t method_len = (size_t)(sp1 - buf);
    size_t target_len = (size_t)(sp2 - sp1 - 1);
    size_t version_len = (size_t)(line_end - sp2 - 1);
    if (method_len == 0 || method_len >= sizeof(req->method)) return 400;
    if (target_len == 0) return 400;
    if (target_len >= sizeof(req->target)) return 414;
    if (version_len == 0 || version_len >= sizeof(req->version)) return 400;
    for (size_t i = 0; i < method_len; i++) {
        if (!((buf[i] >= 'A' && buf[i] <= 'Z') || (buf[i] >= 'a' && buf[i] <= 'z'))) return 400;
    }

    memcpy(req->method, buf, method_len);
    memcpy(req->target, sp1 + 1, target_len);
    memcpy(req->version, sp2 + 1, version_len);
    if (req->target[0] != '/') return 400;  // Only origin-form targets ("/path?query")
    if (strncmp(req->version, "HTTP/1.", 7) != 0) return 505;

    req->headers = line_end + 2;
    req->headers_len = header_len - (size_t)(req->headers - buf);

    // Every header line must be "name: value" with a valid token as the name. A name such as
    // "Content-Length " (space before the colon) could be read differently by the backend.
    for (const char* line = req->headers; line < buf + header_len; ) {
        const char* eol = memchr(line, '\r', (size_t)(buf + header_len - line));
        if (!eol || eol == line) break;  // Blank line: end of the headers
        const char* colon = memchr(line, ':', (size_t)(eol - line));
        if (!colon || colon == line) return 400;
        for (const char* p = line; p < colon; p++) {
            if (!is_token_char(*p)) return 400;
        }
        line = eol + 2;
    }

    char* connection = req->connection;
    int has_connection = 0;
    const char* cursor = req->headers;
    const char *name, *value;
    size_t name_len, value_len;
    while (next_header(&cursor, req->headers + req->headers_len, &name, &name_len, &value, &value_len)) {
        if (header_name_is(name, name_len, "Content-Length")) {
            // A second Content-Length is a classic request-smuggling trick
            char number[32];
            if (req->has_content_length || value_len == 0 || value_len >= sizeof(number)) return 400;
            for (size_t i = 0; i < value_len; i++) {
                if (value[i] < '0' || value[i] > '9') return 400;  // Digits only: no signs or spaces
            }
            memcpy(number, value, value_len);
            number[value_len] = '\0';
            char* number_end;
            errno = 0;
            long n = strtol(number, &number_end, 10);
            if (errno || *number_end != '\0' || n < 0) return 400;
            req->content_length = n;
            req->has_content_length = 1;
        } else if (header_name_is(name, name_len, "Transfer-Encoding")) {
            req->has_transfer_encoding = 1;
        } else if (header_name_is(name, name_len, "Connection")) {
            // Kept whole: the proxy drops every header this lists, so it must not be truncated
            if (value_len >= sizeof(req->connection)) return 400;
            memcpy(connection, value, value_len);
            connection[value_len] = '\0';
            has_connection = 1;
        } else if (header_name_is(name, name_len, "Expect")) {
            req->expect_continue = value_len == 12 && strncasecmp(value, "100-continue", 12) == 0;
        } else if (header_name_is(name, name_len, "Upgrade")) {
            if (value_len >= sizeof(req->upgrade)) return 400;
            memcpy(req->upgrade, value, value_len);
            req->upgrade[value_len] = '\0';
        }
    }

    // An Upgrade header only counts when the Connection header asks for it
    if (!(has_connection && header_has_token(connection, "upgrade"))) req->upgrade[0] = '\0';

    // HTTP/1.1 keeps connections open unless told otherwise; HTTP/1.0 only on request
    if (strcmp(req->version, "HTTP/1.1") == 0) {
        req->keep_alive = !(has_connection && header_has_token(connection, "close"));
    } else {
        req->keep_alive = has_connection && header_has_token(connection, "keep-alive");
    }
    return 0;
}

// ========================================
// HTTP RESPONSES
// ========================================

static const char* status_text_for(int status) {
    switch (status) {
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "Error";
    }
}

// Send a complete response. 'extra_headers' (may be NULL) must end with "\r\n".
// Returns 0 on success, -1 if the client went away.
static int send_response(int sock, int status, const char* status_text, const char* mime_type,
                         const char* body, size_t body_len, int keep_alive, int head_only,
                         const char* extra_headers) {
    char header[2048];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: %s\r\n"
        "Server: WIP/%s\r\n"
        "%s"
        "\r\n",
        status, status_text, mime_type, (unsigned long)body_len,
        keep_alive ? "keep-alive" : "close", WIP_VERSION,
        extra_headers ? extra_headers : "");
    if (header_len < 0 || header_len >= (int)sizeof(header)) return -1;
    if (send_all(sock, header, (size_t)header_len) < 0) return -1;
    if (!head_only && body && body_len > 0) return send_all(sock, body, body_len);
    return 0;
}

// Send a short plain-text error page such as "404 Not Found"
static int send_error(int sock, int status, int keep_alive, int head_only) {
    const char* text = status_text_for(status);
    char body[128];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, text);
    return send_response(sock, status, text, "text/plain; charset=utf-8", body, (size_t)body_len,
                         keep_alive, head_only, NULL);
}

// ========================================
// REVERSE PROXY - ROUTES
// ========================================

typedef struct {
    char prefix[256];          // URL path prefix to match, e.g. "/api" ("/" matches everything)
    char host[256];            // Upstream hostname or IP address
    char port[8];              // Upstream port
    char base_path[512];       // Upstream path that replaces the prefix ("" = upstream root)
    volatile long requests;    // Requests forwarded on this route
    volatile long errors;      // Forwards that failed
} proxy_target_t;

static proxy_target_t proxy_targets[MAX_PROXY_TARGETS];
static int proxy_target_count = 0;

// Add a route. 'prefix' is a path such as "/api" (a trailing "/" or "/*" is ignored).
// 'url' is "http://host[:port][/path]": <prefix>/rest is forwarded to <path>/rest.
static int add_proxy_target(const char* prefix, const char* url) {
    if (proxy_target_count >= MAX_PROXY_TARGETS) {
        fprintf(stderr, "Error: maximum of %d proxy routes reached\n", MAX_PROXY_TARGETS);
        return -1;
    }
    if (prefix[0] != '/') {
        fprintf(stderr, "Error: proxy prefix must start with '/': %s\n", prefix);
        return -1;
    }

    proxy_target_t t;
    memset(&t, 0, sizeof(t));

    // Normalize the prefix: drop a trailing "*" and trailing slashes, but keep a lone "/"
    size_t plen = strlen(prefix);
    if (plen >= sizeof(t.prefix)) {
        fprintf(stderr, "Error: proxy prefix too long: %s\n", prefix);
        return -1;
    }
    memcpy(t.prefix, prefix, plen + 1);
    if (plen > 1 && t.prefix[plen - 1] == '*') t.prefix[--plen] = '\0';
    while (plen > 1 && t.prefix[plen - 1] == '/') t.prefix[--plen] = '\0';

    if (strncasecmp(url, "https://", 8) == 0) {
        fprintf(stderr, "Error: HTTPS upstreams are not supported: %s\n", url);
        return -1;
    }
    if (strncasecmp(url, "http://", 7) != 0) {
        fprintf(stderr, "Error: proxy target must start with http:// : %s\n", url);
        return -1;
    }

    const char* authority = url + 7;
    const char* path = strchr(authority, '/');
    size_t auth_len = path ? (size_t)(path - authority) : strlen(authority);

    // Split host and port; IPv6 literals are written as [addr]:port
    const char* host_start = authority;
    const char* host_end;
    const char* port_start = NULL;
    if (auth_len > 0 && authority[0] == '[') {
        const char* bracket = memchr(authority, ']', auth_len);
        if (!bracket) {
            fprintf(stderr, "Error: invalid IPv6 address in proxy target: %s\n", url);
            return -1;
        }
        host_start = authority + 1;
        host_end = bracket;
        if (bracket + 1 < authority + auth_len && bracket[1] == ':') port_start = bracket + 2;
    } else {
        const char* colon = memchr(authority, ':', auth_len);
        host_end = colon ? colon : authority + auth_len;
        if (colon) port_start = colon + 1;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(t.host)) {
        fprintf(stderr, "Error: invalid host in proxy target: %s\n", url);
        return -1;
    }
    memcpy(t.host, host_start, host_len);

    if (port_start) {
        char port_text[8];
        size_t port_len = (size_t)(authority + auth_len - port_start);
        char* port_end;
        long port = 0;
        if (port_len > 0 && port_len < sizeof(port_text)) {
            memcpy(port_text, port_start, port_len);
            port_text[port_len] = '\0';
            port = strtol(port_text, &port_end, 10);
            if (*port_end != '\0') port = 0;
        }
        if (port < 1 || port > 65535) {
            fprintf(stderr, "Error: invalid port in proxy target: %s\n", url);
            return -1;
        }
        snprintf(t.port, sizeof(t.port), "%ld", port);
    } else {
        strcpy(t.port, "80");
    }

    if (path) {
        size_t path_len = strlen(path);
        if (path_len >= sizeof(t.base_path)) {
            fprintf(stderr, "Error: proxy target path too long: %s\n", url);
            return -1;
        }
        memcpy(t.base_path, path, path_len + 1);
        while (path_len > 0 && t.base_path[path_len - 1] == '/') t.base_path[--path_len] = '\0';
    }

    proxy_targets[proxy_target_count++] = t;
    printf("Proxy route: %s -> http://%s:%s%s\n", t.prefix, t.host, t.port,
           t.base_path[0] ? t.base_path : "/");
    return 0;
}

// Find the route with the longest prefix matching the request target.
// Prefixes match on a path-segment boundary: "/api" matches "/api", "/api/x" and
// "/api?q=1" but not "/apix".
static proxy_target_t* find_proxy_target(const char* target, size_t* prefix_len_out) {
    proxy_target_t* best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < proxy_target_count; i++) {
        const char* prefix = proxy_targets[i].prefix;
        size_t len = strlen(prefix);
        if (strncmp(target, prefix, len) != 0) continue;
        char next = target[len];
        if (!(len == 1 || next == '\0' || next == '/' || next == '?')) continue;
        if (!best || len > best_len) {
            best = &proxy_targets[i];
            best_len = len;
        }
    }
    *prefix_len_out = best_len;
    return best;
}

// ========================================
// CONFIGURATION LOADING
// ========================================

// Settings from earlier versions that no longer exist; ignored with a notice
static const char* removed_settings[] = {
    "enable_memory_pooling", "enable_simd_parsing", "enable_connection_batching",
    "enable_cpu_optimizations", "enable_cache_prefetching", "batch_size",
    "prefetch_distance", "thread_pool_size", NULL
};

// Accepts true/false (and 1/0 for older config files)
static int parse_bool(const char* value, int* out) {
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parse_int(const char* value, int min, int max, int* out) {
    char* end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || end == value || *end != '\0' || n < min || n > max) return 0;
    *out = (int)n;
    return 1;
}

// Extract "key": value from one line of JSON (the config uses one setting per line).
// String values are returned without quotes; an opening object is returned as "{".
static int parse_json_line(const char* line, char* key, size_t key_size, char* value, size_t value_size) {
    const char* p = strchr(line, '"');
    if (!p) return 0;
    p++;
    const char* q = strchr(p, '"');
    if (!q || (size_t)(q - p) >= key_size) return 0;
    memcpy(key, p, (size_t)(q - p));
    key[q - p] = '\0';

    p = q + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"') {
        p++;
        q = strchr(p, '"');
        if (!q) return 0;
    } else if (*p == '{') {
        q = p + 1;
    } else {
        q = p;
        while (*q && *q != ',' && *q != '}' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++;
    }
    if ((size_t)(q - p) >= value_size) return 0;
    memcpy(value, p, (size_t)(q - p));
    value[q - p] = '\0';
    return 1;
}

// Apply one setting. 'section' is "" at the top level, or the enclosing object's name.
static int apply_config_value(const char* section, const char* key, const char* value) {
    if (strcmp(section, "proxy") == 0) {
        return add_proxy_target(key, value) == 0;
    }
    if (section[0] != '\0' && strcmp(section, "performance") != 0) return 0;

    if (strcmp(key, "enable_reverse_proxy") == 0) return parse_bool(value, &config.enable_reverse_proxy);
    if (strcmp(key, "enable_response_caching") == 0) return parse_bool(value, &config.enable_response_caching);
    if (strcmp(key, "enable_statistics") == 0) return parse_bool(value, &config.enable_statistics);
    if (strcmp(key, "enable_event_driven") == 0) return parse_bool(value, &config.enable_event_driven);
    if (strcmp(key, "enable_request_log") == 0) return parse_bool(value, &config.enable_request_log);
    if (strcmp(key, "enable_directory_listing") == 0) return parse_bool(value, &config.enable_directory_listing);
    if (strcmp(key, "buffer_size") == 0) return parse_int(value, 1024, 1024 * 1024, &config.buffer_size);
    if (strcmp(key, "max_connections") == 0) return parse_int(value, 1, 100000, &config.max_connections);
    if (strcmp(key, "max_connections_per_ip") == 0) return parse_int(value, 0, 100000, &config.max_connections_per_ip);
    if (strcmp(key, "keep_alive_timeout") == 0) return parse_int(value, 1, 3600, &config.keep_alive_timeout);
    if (strcmp(key, "proxy_timeout") == 0) return parse_int(value, 1, 3600, &config.proxy_timeout);

    for (int i = 0; removed_settings[i]; i++) {
        if (strcmp(key, removed_settings[i]) == 0) {
            printf("Note: setting \"%s\" is no longer supported and is ignored\n", key);
            return 1;
        }
    }
    return 0;
}

// Load configuration from a JSON file with one setting per line.
// Returns 1 if loaded, 0 if the file doesn't exist, -1 if it contains invalid settings.
static int load_config(const char* config_path) {
    FILE* file = fopen(config_path, "r");
    if (!file) {
        printf("Config file '%s' not found, using defaults\n", config_path);
        return 0;
    }

    char line[1024], key[256], value[768];
    char section[256] = "";
    int line_number = 0, errors = 0;

    while (fgets(line, sizeof(line), file)) {
        line_number++;
        if (!parse_json_line(line, key, sizeof(key), value, sizeof(value))) {
            if (section[0] && strchr(line, '}')) section[0] = '\0';  // End of a nested object
            continue;
        }
        if (strcmp(value, "{") == 0) {
            // Enter a nested object, unless it also closes on this line (e.g. "proxy": {})
            if (!strchr(strchr(line, '{'), '}')) snprintf(section, sizeof(section), "%s", key);
            continue;
        }
        if (!apply_config_value(section, key, value)) {
            fprintf(stderr, "%s:%d: invalid setting \"%s\": %s\n", config_path, line_number, key, value);
            errors++;
        }
    }

    fclose(file);
    if (errors) return -1;
    printf("Configuration loaded from '%s'\n", config_path);
    return 1;
}

// Write a default configuration file. Refuses to overwrite an existing file.
static int create_default_config(const char* config_path) {
    FILE* existing = fopen(config_path, "r");
    if (existing) {
        fclose(existing);
        fprintf(stderr, "Config file '%s' already exists; not overwriting it\n", config_path);
        return -1;
    }

    FILE* file = fopen(config_path, "w");
    if (!file) {
        fprintf(stderr, "Could not create config file '%s'\n", config_path);
        return -1;
    }
    fputs("{\n"
          "  \"enable_reverse_proxy\": true,\n"
          "  \"enable_response_caching\": true,\n"
          "  \"enable_statistics\": true,\n"
          "  \"enable_request_log\": true,\n"
          "  \"enable_directory_listing\": true,\n"
          "\n"
          "  \"performance\": {\n"
          "    \"enable_event_driven\": false\n"
          "  },\n"
          "\n"
          "  \"proxy\": {\n"
          "  },\n"
          "\n"
          "  \"buffer_size\": 8192,\n"
          "  \"max_connections\": 1000,\n"
          "  \"max_connections_per_ip\": 64,\n"
          "  \"keep_alive_timeout\": 30,\n"
          "  \"proxy_timeout\": 30\n"
          "}\n", file);
    fclose(file);
    printf("Default configuration created: '%s'\n", config_path);
    return 0;
}

// ========================================
// REVERSE PROXY - FORWARDING
// ========================================

// Growable string buffer used to assemble header blocks
typedef struct {
    char* data;
    size_t len;
    size_t cap;
    int failed;
} strbuf_t;

static void sb_append(strbuf_t* sb, const char* s, size_t n) {
    if (sb->failed) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 1024;
        while (cap < sb->len + n + 1) cap *= 2;
        char* grown = realloc(sb->data, cap);
        if (!grown) {
            sb->failed = 1;
            return;
        }
        sb->data = grown;
        sb->cap = cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_add(strbuf_t* sb, const char* s) {
    sb_append(sb, s, strlen(s));
}

static void sb_add_header(strbuf_t* sb, const char* name, size_t name_len, const char* value, size_t value_len) {
    sb_append(sb, name, name_len);
    sb_append(sb, ": ", 2);
    sb_append(sb, value, value_len);
    sb_append(sb, "\r\n", 2);
}

// Headers that only apply to a single connection and must not be forwarded
static int is_hop_by_hop(const char* name, size_t name_len) {
    static const char* hop_by_hop[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authenticate",
        "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding", "Upgrade", NULL
    };
    for (int i = 0; hop_by_hop[i]; i++) {
        if (header_name_is(name, name_len, hop_by_hop[i])) return 1;
    }
    return 0;
}

// Connect to the route's upstream (hostname or IP, IPv4 or IPv6) within a timeout.
// Returns a connected blocking socket, or -1.
static int connect_upstream(const proxy_target_t* t, int timeout_seconds) {
    struct addrinfo hints, *results = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(t->host, t->port, &hints, &results) != 0) return -1;

    int sock = -1;
    for (struct addrinfo* ai = results; ai; ai = ai->ai_next) {
        sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;

        // Non-blocking connect so we can give up after the timeout
        set_socket_blocking(sock, 0);
        int rc = connect(sock, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        int connected = rc == 0;
#ifdef _WIN32
        if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_set, error_set;
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET((SOCKET)sock, &write_set);
            FD_SET((SOCKET)sock, &error_set);
            struct timeval tv = { timeout_seconds, 0 };
            connected = select(0, NULL, &write_set, &error_set, &tv) > 0 && FD_ISSET((SOCKET)sock, &write_set);
        }
#else
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = sock, .events = POLLOUT };
            int ready;
            do {
                ready = poll(&pfd, 1, timeout_seconds * 1000);
            } while (ready < 0 && errno == EINTR);
            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            connected = ready > 0 && getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 && so_error == 0;
        }
#endif
        if (connected) {
            set_socket_blocking(sock, 1);
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(results);
    return sock;
}

// After a 101 Switching Protocols (e.g. a WebSocket), copy bytes in both directions
// until either side closes the connection.
static void tunnel(int client, int upstream) {
    char chunk[IO_CHUNK_SIZE];
    int socks[2] = { client, upstream };
    for (;;) {
        int ready[2];
#ifdef _WIN32
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET((SOCKET)client, &read_set);
        FD_SET((SOCKET)upstream, &read_set);
        if (select(0, &read_set, NULL, NULL, NULL) <= 0) return;
        ready[0] = FD_ISSET((SOCKET)client, &read_set);
        ready[1] = FD_ISSET((SOCKET)upstream, &read_set);
#else
        struct pollfd fds[2] = { { .fd = client, .events = POLLIN }, { .fd = upstream, .events = POLLIN } };
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        ready[0] = fds[0].revents != 0;
        ready[1] = fds[1].revents != 0;
#endif
        for (int i = 0; i < 2; i++) {
            if (!ready[i]) continue;
            ssize_t n = recv(socks[i], chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            if (send_all(socks[1 - i], chunk, (size_t)n) < 0) return;
        }
    }
}

// Forward one request to the route's upstream and relay the response to the client.
// 'body' holds the first 'body_in_buf' bytes of the request body; the rest is read
// from the client. Returns 1 if the client connection can stay open, 0 to close it.
// '*status_out' is set to the status sent to the client (0 if none was sent).
static int proxy_request(int client, const http_request_t* req, const char* body, size_t body_in_buf,
                         proxy_target_t* t, size_t prefix_len, const char* client_ip, int keep_alive,
                         int* status_out) {
    int head_only = strcmp(req->method, "HEAD") == 0;
    long body_remaining = req->content_length - (long)body_in_buf;
    int upstream = -1;
    int result = 0;
    int error_status = 502;
    strbuf_t out = {0};
    char* resp = NULL;
    size_t resp_len = 0, resp_header_len = 0;
    char line[MAX_PATH_LEN + 1024];
    char chunk[IO_CHUNK_SIZE];
    char original_host[256] = "";
    char forwarded_for[512] = "";
    char header_value[64];
    const char *cursor, *name, *value;
    size_t name_len, value_len;

    ATOMIC_INC(t->requests);
    if (config.enable_statistics) ATOMIC_INC(proxy_requests);
    *status_out = 0;

    upstream = connect_upstream(t, config.proxy_timeout);
    if (upstream < 0) {
        fprintf(stderr, "Proxy: cannot connect to %s:%s for %s\n", t->host, t->port, req->target);
        goto fail;
    }
    set_socket_timeouts(upstream, config.proxy_timeout);

    // Request line: route base path + whatever follows the prefix (query string included)
    const char* rest = (prefix_len == 1) ? req->target : req->target + prefix_len;
    int need_slash = t->base_path[0] == '\0' && rest[0] != '/';
    snprintf(line, sizeof(line), "%s %s%s%s HTTP/1.1\r\n", req->method, need_slash ? "/" : "", t->base_path, rest);
    sb_add(&out, line);

    // Client headers, minus hop-by-hop headers and the ones we set ourselves. Names with
    // underscores are dropped (as nginx does): PHP, WSGI and CGI backends turn "X_Forwarded_For"
    // into the same variable as "X-Forwarded-For", which would let clients spoof it.
    cursor = req->headers;
    while (next_header(&cursor, req->headers + req->headers_len, &name, &name_len, &value, &value_len)) {
        if (memchr(name, '_', name_len)) continue;
        // Headers the client lists in Connection are meant for this hop only (RFC 9110)
        if (req->connection[0] && name_len < 64) {
            char header_name[64];
            memcpy(header_name, name, name_len);
            header_name[name_len] = '\0';
            if (header_has_token(req->connection, header_name)) continue;
        }
        if (header_name_is(name, name_len, "Host")) {
            snprintf(original_host, sizeof(original_host), "%.*s", (int)value_len, value);
        } else if (header_name_is(name, name_len, "X-Forwarded-For")) {
            snprintf(forwarded_for, sizeof(forwarded_for), "%.*s, ", (int)value_len, value);
        } else if (!is_hop_by_hop(name, name_len) &&
                   !header_name_is(name, name_len, "Content-Length") &&
                   !header_name_is(name, name_len, "Expect") &&
                   !header_name_is(name, name_len, "X-Forwarded-Proto") &&
                   !header_name_is(name, name_len, "X-Forwarded-Host")) {
            sb_add_header(&out, name, name_len, value, value_len);
        }
    }

    int ipv6_host = strchr(t->host, ':') != NULL;
    int default_port = strcmp(t->port, "80") == 0;
    snprintf(line, sizeof(line), "Host: %s%s%s%s%s\r\n",
             ipv6_host ? "[" : "", t->host, ipv6_host ? "]" : "",
             default_port ? "" : ":", default_port ? "" : t->port);
    sb_add(&out, line);
    snprintf(line, sizeof(line), "X-Forwarded-For: %s%s\r\nX-Forwarded-Proto: http\r\n", forwarded_for, client_ip);
    sb_add(&out, line);
    if (original_host[0]) {
        snprintf(line, sizeof(line), "X-Forwarded-Host: %s\r\n", original_host);
        sb_add(&out, line);
    }
    if (req->has_content_length) {
        snprintf(line, sizeof(line), "Content-Length: %ld\r\n", req->content_length);
        sb_add(&out, line);
    }
    if (req->upgrade[0]) {
        // WebSocket and other protocol upgrades keep the upstream connection open
        snprintf(line, sizeof(line), "Upgrade: %s\r\nConnection: Upgrade\r\n\r\n", req->upgrade);
        sb_add(&out, line);
    } else {
        sb_add(&out, "Connection: close\r\n\r\n");
    }
    if (out.failed || send_all(upstream, out.data, out.len) < 0) goto fail;

    // Request body: the part already buffered, then the rest straight from the client
    if (body_in_buf > 0 && send_all(upstream, body, body_in_buf) < 0) goto fail;
    if (body_remaining > 0 && body_in_buf == 0 && req->expect_continue) {
        static const char continue_response[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_all(client, continue_response, sizeof(continue_response) - 1) < 0) goto done;
    }
    // A body that slows to a trickle is cut off: it must keep arriving at MIN_UPLOAD_RATE,
    // checked over windows of the request deadline. Large uploads on slow links still work.
    double window_start = now_ms();
    long window_bytes = 0;
    while (body_remaining > 0) {
        int want = body_remaining > (long)sizeof(chunk) ? (int)sizeof(chunk) : (int)body_remaining;
        ssize_t n = recv(client, chunk, want, 0);
        if (n <= 0) goto done;  // Client went away mid-body
        body_remaining -= (long)n;
        if (send_all(upstream, chunk, (size_t)n) < 0) goto fail;
        window_bytes += (long)n;
        double window_ms = now_ms() - window_start;
        if (window_ms >= request_deadline_seconds() * 1000.0) {
            if (window_bytes < (long)(MIN_UPLOAD_RATE * window_ms / 1000.0)) {
                error_status = 408;
                goto fail;
            }
            window_start = now_ms();
            window_bytes = 0;
        }
    }

    // Read the upstream's response header block
    resp = malloc(PROXY_HEADER_BUFFER);
    if (!resp) goto fail;
    while ((resp_header_len = find_header_end(resp, resp_len)) == 0) {
        if (resp_len == PROXY_HEADER_BUFFER) goto fail;  // Header block too large
        ssize_t n = recv(upstream, resp + resp_len, (int)(PROXY_HEADER_BUFFER - resp_len), 0);
        if (n <= 0) {
            if (n < 0 && socket_timed_out()) error_status = 504;
            goto fail;
        }
        resp_len += (size_t)n;
    }

    // Status line: "HTTP/1.x NNN Reason"
    if (resp_header_len < 12 || strncmp(resp, "HTTP/1.", 7) != 0 || resp[8] != ' ' ||
        resp[9] < '1' || resp[9] > '5' || resp[10] < '0' || resp[10] > '9' ||
        resp[11] < '0' || resp[11] > '9') {
        goto fail;
    }
    int status = (resp[9] - '0') * 100 + (resp[10] - '0') * 10 + (resp[11] - '0');

    // Upgrade accepted: pass the upstream's response through as-is, then just copy bytes
    if (req->upgrade[0] && status == 101) {
        *status_out = 101;
        if (send_all(client, resp, resp_len) == 0) tunnel(client, upstream);
        if (config.enable_statistics) ATOMIC_INC(successful_requests);
        result = 0;  // The client connection now belongs to the other protocol
        goto done;
    }
    const char* status_end = memchr(resp, '\r', resp_header_len);
    const char* resp_headers = status_end + 2;
    size_t resp_headers_len = resp_header_len - (size_t)(resp_headers - resp);

    // Work out how the body ends, which decides whether the client connection can be reused
    long resp_content_length = -1;
    if (find_header(resp_headers, resp_headers_len, "Content-Length", header_value, sizeof(header_value))) {
        char* number_end;
        long n = strtol(header_value, &number_end, 10);
        if (number_end != header_value && *number_end == '\0' && n >= 0) resp_content_length = n;
    }
    int resp_chunked = find_header(resp_headers, resp_headers_len, "Transfer-Encoding",
                                   header_value, sizeof(header_value)) &&
                       header_has_token(header_value, "chunked");
    // With both, chunked encoding wins (RFC 9112): ignore Content-Length and don't pass it on
    if (resp_chunked) resp_content_length = -1;
    int no_body = head_only || status == 204 || status == 304;
    int client_keep = keep_alive && !req->upgrade[0] && status >= 200 &&
                      (no_body || resp_content_length >= 0 || resp_chunked);

    // Relay the status line and headers with our own Connection header
    out.len = 0;
    sb_append(&out, resp, (size_t)(status_end - resp));
    sb_add(&out, "\r\n");
    cursor = resp_headers;
    while (next_header(&cursor, resp_headers + resp_headers_len, &name, &name_len, &value, &value_len)) {
        if (!header_name_is(name, name_len, "Connection") &&
            !header_name_is(name, name_len, "Keep-Alive") &&
            !header_name_is(name, name_len, "Proxy-Connection") &&
            !(resp_chunked && header_name_is(name, name_len, "Content-Length"))) {
            sb_add_header(&out, name, name_len, value, value_len);
        }
    }
    sb_add(&out, client_keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
    if (out.failed) goto fail;
    *status_out = status;
    if (send_all(client, out.data, out.len) < 0) goto done;

    // Relay the body: exactly Content-Length bytes, otherwise until the upstream closes
    if (!no_body) {
        long remaining = resp_content_length;  // -1 means "until the upstream closes"
        size_t buffered = resp_len - resp_header_len;
        if (remaining >= 0 && (long)buffered > remaining) buffered = (size_t)remaining;
        if (buffered > 0) {
            if (send_all(client, resp + resp_header_len, buffered) < 0) goto done;
            if (remaining >= 0) remaining -= (long)buffered;
        }
        while (remaining != 0) {
            int want = (remaining > 0 && remaining < (long)sizeof(chunk)) ? (int)remaining : (int)sizeof(chunk);
            ssize_t n = recv(upstream, chunk, want, 0);
            if (n == 0 && remaining < 0) break;  // Upstream closed: end of a close-delimited or chunked body
            if (n <= 0) goto done;               // Truncated response: drop the client connection
            if (send_all(client, chunk, (size_t)n) < 0) goto done;
            if (remaining > 0) remaining -= (long)n;
        }
    }

    if (config.enable_statistics) ATOMIC_INC(successful_requests);
    result = client_keep;
    goto done;

fail:
    // Nothing has been sent to the client yet, so it gets a proper error response
    ATOMIC_INC(t->errors);
    if (config.enable_statistics) ATOMIC_INC(proxy_errors);
    result = keep_alive && body_remaining == 0;
    *status_out = error_status;
    if (send_error(client, error_status, result, head_only) < 0) result = 0;

done:
    if (upstream >= 0) close(upstream);
    free(out.data);
    free(resp);
    return result;
}



// ========================================
// RESPONSE CACHING MODULE
// ========================================

// Response caching support - always compiled, runtime configurable

#define CACHE_SIZE 1024
typedef struct {
    char path[MAX_PATH_LEN];
    char* response;
    size_t response_len;
    time_t mtime;      // File modification time when cached
    long file_size;    // File size when cached
    int hits;
} cache_entry_t;

static cache_entry_t response_cache[CACHE_SIZE];
static int cache_initialized = 0;

#ifdef _WIN32
static CRITICAL_SECTION cache_lock;
#else
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

// Initialize cache system
void init_cache() {
    if (!cache_initialized) {
#ifdef _WIN32
        InitializeCriticalSection(&cache_lock);
#endif
        memset(response_cache, 0, sizeof(response_cache));
        cache_initialized = 1;
        printf("Response caching initialized (%d entries)\n", CACHE_SIZE);
    }
}

// Get cached response as a malloc'd copy (caller frees). Copying under the lock
// prevents another thread from freeing the entry while it is being sent.
// Entries are only used while the file's modification time and size are unchanged.
char* get_cached_response(const char* path, time_t mtime, long file_size, size_t* out_len) {
    if (!cache_initialized) return NULL;
    char* copy = NULL;
    
    uint32_t hash = 0;
    for (const char* p = path; *p; p++) {
        hash = hash * 31 + *p;
    }
    int index = hash % CACHE_SIZE;
    
#ifdef _WIN32
    EnterCriticalSection(&cache_lock);
#else
    pthread_mutex_lock(&cache_lock);
#endif
    
    cache_entry_t* entry = &response_cache[index];
    if (entry->response && strcmp(entry->path, path) == 0 &&
        entry->mtime == mtime && entry->file_size == file_size) {
        entry->hits++;
        copy = malloc(entry->response_len);
        if (copy) {
            memcpy(copy, entry->response, entry->response_len);
            *out_len = entry->response_len;
        }
    }
    
#ifdef _WIN32
    LeaveCriticalSection(&cache_lock);
#else
    pthread_mutex_unlock(&cache_lock);
#endif
    return copy;
}

// Cache a complete response for a file with the given modification time and size
void cache_response(const char* path, time_t mtime, long file_size, const char* response, size_t response_len) {
    if (!cache_initialized || response_len > CACHE_MAX_FILE_SIZE + 2048) return; // Don't cache large responses
    
    uint32_t hash = 0;
    for (const char* p = path; *p; p++) {
        hash = hash * 31 + *p;
    }
    int index = hash % CACHE_SIZE;
    
#ifdef _WIN32
    EnterCriticalSection(&cache_lock);
#else
    pthread_mutex_lock(&cache_lock);
#endif
    
    cache_entry_t* entry = &response_cache[index];
    
    // Free old response if exists
    if (entry->response) {
        free(entry->response);
        entry->response = NULL;
    }
    
    // Cache new response
    strncpy(entry->path, path, MAX_PATH_LEN - 1);
    entry->path[MAX_PATH_LEN - 1] = '\0';
    entry->response = malloc(response_len);
    if (entry->response) {
        memcpy(entry->response, response, response_len);
        entry->response_len = response_len;
        entry->mtime = mtime;
        entry->file_size = file_size;
        entry->hits = 0;
    }
    
#ifdef _WIN32
    LeaveCriticalSection(&cache_lock);
#else
    pthread_mutex_unlock(&cache_lock);
#endif
}

// Cleanup cache
void cleanup_cache() {
    if (!cache_initialized) return;
    
#ifdef _WIN32
    EnterCriticalSection(&cache_lock);
#else
    pthread_mutex_lock(&cache_lock);
#endif
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (response_cache[i].response) {
            free(response_cache[i].response);
            response_cache[i].response = NULL;
        }
    }
    
#ifdef _WIN32
    LeaveCriticalSection(&cache_lock);
    DeleteCriticalSection(&cache_lock);
#else
    pthread_mutex_unlock(&cache_lock);
    pthread_mutex_destroy(&cache_lock);
#endif
    
    cache_initialized = 0;
}

// ========================================
// EVENT BACKEND (EXPERIMENTAL)
// ========================================
//
// Creates the platform's event notification handle: epoll on Linux, kqueue on
// macOS/BSD and an I/O completion port on Windows. This is the starting point
// for an event-driven connection loop; it is NOT used to handle requests yet,
// which are still served one thread per connection.
// Enabled with "enable_event_driven": true in the config file.

#if defined(_WIN32)
static HANDLE event_handle = NULL;
#elif defined(__linux__)
#include <sys/epoll.h>
static int event_fd = -1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
static int event_fd = -1;
#endif

// Create the event handle. Returns the backend name, or NULL if unavailable.
static const char* init_event_backend(void) {
#if defined(_WIN32)
    event_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    return event_handle ? "IOCP" : NULL;
#elif defined(__linux__)
    event_fd = epoll_create1(EPOLL_CLOEXEC);
    return event_fd >= 0 ? "epoll" : NULL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    event_fd = kqueue();
    return event_fd >= 0 ? "kqueue" : NULL;
#else
    return NULL;
#endif
}

static void cleanup_event_backend(void) {
#if defined(_WIN32)
    if (event_handle) {
        CloseHandle(event_handle);
        event_handle = NULL;
    }
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (event_fd >= 0) {
        close(event_fd);
        event_fd = -1;
    }
#endif
}

// ========================================
// STATIC FILE SERVING
// ========================================

typedef struct {
    const char* ext;
    const char* mime;
} mime_type_t;

static const mime_type_t mime_types[] = {
    {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"}, {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"}, {"json", "application/json"},
    {"map", "application/json"}, {"webmanifest", "application/manifest+json"},
    {"xml", "application/xml"}, {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"}, {"csv", "text/csv; charset=utf-8"},
    {"wasm", "application/wasm"},
    {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
    {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"ico", "image/x-icon"},
    {"webp", "image/webp"}, {"avif", "image/avif"},
    {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"}, {"otf", "font/otf"},
    {"mp4", "video/mp4"}, {"webm", "video/webm"},
    {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"},
    {"pdf", "application/pdf"}, {"zip", "application/zip"},
    {NULL, NULL}
};

// Get MIME type for file extension
static const char* get_mime_type(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    ext++; // Skip the dot

    for (int i = 0; mime_types[i].ext; i++) {
        if (strcasecmp(ext, mime_types[i].ext) == 0) {
            return mime_types[i].mime;
        }
    }
    return "application/octet-stream";
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode %XX escapes in a URL path. Rejects malformed escapes and encoded NUL bytes.
static int url_decode(const char* in, char* out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        char c = in[i];
        if (c == '%') {
            int hi = hex_value(in[i + 1]);
            if (hi < 0) return -1;
            int lo = hex_value(in[i + 2]);
            if (lo < 0) return -1;
            c = (char)(hi * 16 + lo);
            if (c == '\0') return -1;
            i += 2;
        }
        if (o + 1 >= out_size) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

// Built-in /__stats endpoint
static int send_stats(int sock, int keep_alive, int head_only) {
    if (!config.enable_statistics) {
        static const char disabled[] = "{\"message\":\"Statistics disabled\"}";
        return send_response(sock, 200, "OK", "application/json", disabled, sizeof(disabled) - 1,
                             keep_alive, head_only, NULL);
    }
    double uptime = difftime(time(NULL), server_start_time);
    double rps = uptime > 0 ? (double)total_requests / uptime : 0;
    char body[1024];
    int body_len = snprintf(body, sizeof(body),
        "{\n"
        "  \"server\": \"WIP\",\n"
        "  \"version\": \"%s\",\n"
        "  \"uptime_seconds\": %.0f,\n"
        "  \"total_requests\": %ld,\n"
        "  \"successful_requests\": %ld,\n"
        "  \"requests_per_second\": %.2f,\n"
        "  \"active_connections\": %ld,\n"
        "  \"proxy_routes\": %d,\n"
        "  \"proxy_requests\": %ld,\n"
        "  \"proxy_errors\": %ld\n"
        "}\n",
        WIP_VERSION, uptime, total_requests, successful_requests, rps,
        active_connection_threads, proxy_target_count, proxy_requests, proxy_errors);
    return send_response(sock, 200, "OK", "application/json", body, (size_t)body_len,
                         keep_alive, head_only, NULL);
}

// Parse a single "bytes=" range for a file of 'size' bytes into an inclusive [start, end].
// Returns 1 for a usable range, 0 if the header should be ignored (other units, several
// ranges or malformed - the whole file is sent instead), -1 if it can't be satisfied.
static int parse_range(const char* value, long size, long* start, long* end) {
    if (strncasecmp(value, "bytes=", 6) != 0) return 0;
    const char* spec = value + 6;
    if (strchr(spec, ',')) return 0;
    const char* dash = strchr(spec, '-');
    if (!dash) return 0;
    char* number_end;

    if (dash == spec) {
        // "bytes=-N": the last N bytes
        errno = 0;
        long n = strtol(dash + 1, &number_end, 10);
        if (errno || number_end == dash + 1 || *number_end != '\0' || n < 0) return 0;
        if (n == 0 || size == 0) return -1;
        if (n > size) n = size;
        *start = size - n;
        *end = size - 1;
        return 1;
    }

    errno = 0;
    long first = strtol(spec, &number_end, 10);
    if (errno || number_end != dash || first < 0) return 0;
    long last = size - 1;
    if (dash[1] != '\0') {
        errno = 0;
        last = strtol(dash + 1, &number_end, 10);
        if (errno || *number_end != '\0' || last < first) return 0;
        if (last > size - 1) last = size - 1;
    }
    if (first >= size) return -1;
    *start = first;
    *end = last;
    return 1;
}

// Send 'length' bytes of 'file' starting at 'offset'. Returns 0 on success, -1 on error,
// including when the file is shorter than expected (it shrank while being sent).
// Linux and macOS use sendfile() so the kernel copies straight from the file to the socket.
// Windows keeps a read/send loop: TransmitFile only runs two at a time on desktop Windows.
static int send_file_range(int sock, FILE* file, long offset, long length) {
#if defined(__linux__)
    int fd = fileno(file);
    off_t position = offset;
    while (length > 0) {
        size_t want = length > (1L << 30) ? (size_t)(1L << 30) : (size_t)length;
        ssize_t n = sendfile(sock, fd, &position, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;  // Error, send timeout, or end of file
        length -= (long)n;
    }
    return 0;
#elif defined(__APPLE__)
    int fd = fileno(file);
    off_t position = offset;
    while (length > 0) {
        off_t sent = length > (1L << 30) ? (off_t)(1L << 30) : (off_t)length;
        int rc = sendfile(fd, sock, position, &sent, NULL, 0);
        position += sent;
        length -= (long)sent;
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;  // Error or send timeout
        }
        if (sent == 0 && length > 0) return -1;  // End of file
    }
    return 0;
#else
    char chunk[IO_CHUNK_SIZE];
    if (fseek(file, offset, SEEK_SET) != 0) return -1;
    while (length > 0) {
        size_t want = length < (long)sizeof(chunk) ? (size_t)length : sizeof(chunk);
        size_t n = fread(chunk, 1, want, file);
        if (n == 0) return -1;
        if (send_all(sock, chunk, n) < 0) return -1;
        length -= (long)n;
    }
    return 0;
#endif
}

// Send a file, or the part of it selected by 'range' (a Range header value, or NULL).
// Small whole files are served from (and added to) the response cache.
// Returns 0 on success, -1 if the connection must be closed. Sets '*status' to the status sent.
static int serve_file(int sock, const char* filepath, const struct stat* st, const char* range,
                      int keep_alive, int head_only, int* status) {
    const char* mime_type = get_mime_type(filepath);
    long file_size = (long)st->st_size;
    long range_start = 0, range_end = file_size - 1;
    int partial = 0;
    *status = 200;

    if (range) {
        int parsed = parse_range(range, file_size, &range_start, &range_end);
        if (parsed < 0) {
            static const char body[] = "416 Range Not Satisfiable\n";
            char content_range[64];
            snprintf(content_range, sizeof(content_range), "Content-Range: bytes */%ld\r\n", file_size);
            *status = 416;
            return send_response(sock, 416, "Range Not Satisfiable", "text/plain; charset=utf-8",
                                 body, sizeof(body) - 1, keep_alive, head_only, content_range);
        }
        partial = parsed == 1;
    }

    // Cached responses include the Connection header, so key on keep-alive state too
    char cache_key[MAX_PATH_LEN + 4];
    snprintf(cache_key, sizeof(cache_key), "%s|%c", filepath, keep_alive ? 'k' : 'c');

    if (config.enable_response_caching && !head_only && !partial) {
        size_t cached_len = 0;
        char* cached = get_cached_response(cache_key, st->st_mtime, file_size, &cached_len);
        if (cached) {
            int rc = send_all(sock, cached, cached_len);
            free(cached);
            return rc;
        }
    }

    FILE* file = fopen(filepath, "rb");
    if (!file) {
        *status = 404;
        return send_error(sock, 404, keep_alive, head_only);
    }

    long length = range_end - range_start + 1;
    char header[1024];
    int header_len;
    if (partial) {
        *status = 206;
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: %s\r\n"
            "Server: WIP/%s\r\n"
            "\r\n",
            mime_type, length, range_start, range_end, file_size,
            keep_alive ? "keep-alive" : "close", WIP_VERSION);
    } else {
        length = file_size;
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: %s\r\n"
            "Server: WIP/%s\r\n"
            "\r\n",
            mime_type, file_size, keep_alive ? "keep-alive" : "close", WIP_VERSION);
    }

    if (head_only) {
        fclose(file);
        return send_all(sock, header, (size_t)header_len);
    }

    // Small whole files: build the complete response once and cache it
    if (!partial && config.enable_response_caching && file_size < CACHE_MAX_FILE_SIZE) {
        size_t total = (size_t)header_len + (size_t)file_size;
        char* full = malloc(total);
        if (full) {
            memcpy(full, header, (size_t)header_len);
            if (fread(full + header_len, 1, (size_t)file_size, file) == (size_t)file_size) {
                cache_response(cache_key, st->st_mtime, file_size, full, total);
                int rc = send_all(sock, full, total);
                free(full);
                fclose(file);
                return rc;
            }
            free(full);
            fseek(file, 0, SEEK_SET);
        }
    }

    if (partial && fseek(file, range_start, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    // Stream the file (or the requested part)
    int rc = send_all(sock, header, (size_t)header_len);
    if (rc == 0) rc = send_file_range(sock, file, partial ? range_start : 0, length);
    fclose(file);
    return rc;
}

// True if any folder or file name in the part of 'resolved' below 'root' starts with a dot.
// Checking the resolved path catches hidden files reached through symlinks or short names.
static int has_hidden_segment(const char* resolved, const char* root) {
    const char* p = resolved + strlen(root);
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (*p == '.') return 1;
        while (*p && *p != '/' && *p != '\\') p++;
    }
    return 0;
}

// Send an error page from serve_static and record its status for the request log.
// Returns 1 if the connection can stay open, 0 if it must be closed.
static int static_error(int sock, int code, int keep_alive, int head_only, int* status) {
    *status = code;
    return send_error(sock, code, keep_alive, head_only) == 0 && keep_alive;
}

typedef struct {
    char* name;
    int is_dir;
    double size;
} dir_entry_t;

static int compare_entries(const void* a, const void* b) {
    const dir_entry_t* x = (const dir_entry_t*)a;
    const dir_entry_t* y = (const dir_entry_t*)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;  // Folders first
    return strcasecmp(x->name, y->name);
}

// Append text with HTML special characters escaped
static void sb_add_html(strbuf_t* sb, const char* s) {
    for (; *s; s++) {
        switch (*s) {
            case '&': sb_add(sb, "&amp;"); break;
            case '<': sb_add(sb, "&lt;"); break;
            case '>': sb_add(sb, "&gt;"); break;
            case '"': sb_add(sb, "&quot;"); break;
            case '\'': sb_add(sb, "&#39;"); break;
            default: sb_append(sb, s, 1);
        }
    }
}

// Append a file name percent-encoded for use in a link
static void sb_add_url(strbuf_t* sb, const char* s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            sb_append(sb, s, 1);
        } else {
            char escaped[3] = { '%', hex[c >> 4], hex[c & 15] };
            sb_append(sb, escaped, 3);
        }
    }
}

// Read the visible (non-hidden) entries of a directory. Returns the count, or -1 on error.
static int read_directory(const char* dir_path, dir_entry_t** out) {
    int count = 0, capacity = 64;
    dir_entry_t* entries = malloc((size_t)capacity * sizeof(dir_entry_t));
    if (!entries) return -1;
#ifdef _WIN32
    char pattern[MAX_PATH_LEN + 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir_path);
    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) {
        free(entries);
        return -1;
    }
    do {
        const char* name = found.cFileName;
        int is_dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        double size = (double)found.nFileSizeHigh * 4294967296.0 + (double)found.nFileSizeLow;
#else
    DIR* dir = opendir(dir_path);
    if (!dir) {
        free(entries);
        return -1;
    }
    struct dirent* item;
    while ((item = readdir(dir)) != NULL) {
        const char* name = item->d_name;
        char full_path[MAX_PATH_LEN * 2];
        struct stat item_stat;
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
        if (stat(full_path, &item_stat) != 0) continue;
        int is_dir = S_ISDIR(item_stat.st_mode);
        double size = (double)item_stat.st_size;
#endif
        if (name[0] == '.') continue;  // Hidden entries (and "."/"..") are never served, so don't list them
        if (count == capacity) {
            capacity *= 2;
            dir_entry_t* grown = realloc(entries, (size_t)capacity * sizeof(dir_entry_t));
            if (!grown) break;
            entries = grown;
        }
        entries[count].name = malloc(strlen(name) + 1);
        if (!entries[count].name) break;
        strcpy(entries[count].name, name);
        entries[count].is_dir = is_dir;
        entries[count].size = size;
        count++;
#ifdef _WIN32
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    }
    closedir(dir);
#endif
    qsort(entries, (size_t)count, sizeof(dir_entry_t), compare_entries);
    *out = entries;
    return count;
}

// Send an HTML listing of a folder. 'url_path' is the decoded request path, ending in "/".
static int send_directory_listing(int sock, const char* dir_path, const char* url_path,
                                  int keep_alive, int head_only) {
    dir_entry_t* entries = NULL;
    int count = read_directory(dir_path, &entries);
    if (count < 0) return send_error(sock, 500, keep_alive, head_only);

    strbuf_t page = {0};
    sb_add(&page, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Index of ");
    sb_add_html(&page, url_path);
    sb_add(&page, "</title>\n<style>body{font-family:system-ui,sans-serif;margin:2rem}"
                  "td{padding:.15rem 2rem .15rem 0}td:last-child{text-align:right;color:#666}</style>\n"
                  "</head><body>\n<h1>Index of ");
    sb_add_html(&page, url_path);
    sb_add(&page, "</h1>\n<table>\n");
    if (strcmp(url_path, "/") != 0) sb_add(&page, "<tr><td><a href=\"../\">../</a></td><td></td></tr>\n");
    for (int i = 0; i < count; i++) {
        char size_text[32] = "";
        if (!entries[i].is_dir) {
            double size = entries[i].size;
            if (size < 1024) snprintf(size_text, sizeof(size_text), "%.0f B", size);
            else if (size < 1024.0 * 1024) snprintf(size_text, sizeof(size_text), "%.1f KB", size / 1024);
            else if (size < 1024.0 * 1024 * 1024) snprintf(size_text, sizeof(size_text), "%.1f MB", size / (1024.0 * 1024));
            else snprintf(size_text, sizeof(size_text), "%.1f GB", size / (1024.0 * 1024 * 1024));
        }
        sb_add(&page, "<tr><td><a href=\"");
        sb_add_url(&page, entries[i].name);
        if (entries[i].is_dir) sb_add(&page, "/");
        sb_add(&page, "\">");
        sb_add_html(&page, entries[i].name);
        if (entries[i].is_dir) sb_add(&page, "/");
        sb_add(&page, "</a></td><td>");
        sb_add(&page, size_text);
        sb_add(&page, "</td></tr>\n");
        free(entries[i].name);
    }
    free(entries);
    sb_add(&page, "</table>\n</body></html>\n");

    int rc = page.failed ? send_error(sock, 500, keep_alive, head_only)
                         : send_response(sock, 200, "OK", "text/html; charset=utf-8", page.data, page.len,
                                         keep_alive, head_only, NULL);
    free(page.data);
    return rc;
}

// Serve a GET or HEAD request from the served directory. '*status' is set to the status sent.
// Returns 1 if the connection can stay open, 0 if it must be closed.
static int serve_static(int sock, const http_request_t* req, int keep_alive, int* status) {
    int head_only = strcmp(req->method, "HEAD") == 0;
    *status = 200;
    if (!head_only && strcmp(req->method, "GET") != 0) {
        static const char body[] = "405 Method Not Allowed\n";
        *status = 405;
        return send_response(sock, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                             body, sizeof(body) - 1, keep_alive, 0, "Allow: GET, HEAD\r\n") == 0 && keep_alive;
    }

    // Split off the query string, then decode %XX escapes
    char raw_path[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    snprintf(raw_path, sizeof(raw_path), "%s", req->target);
    char* query = strchr(raw_path, '?');
    if (query) *query = '\0';
    if (url_decode(raw_path, path, sizeof(path)) < 0) {
        return static_error(sock, 400, keep_alive, head_only, status);
    }

    if (strcmp(path, "/__stats") == 0) {
        return send_stats(sock, keep_alive, head_only) == 0 && keep_alive;
    }

    // Refuse backslashes and hidden files or directories (any segment starting with '.').
    // This also rules out ".." traversal and files such as .git/config or .env.
    int forbidden = strchr(path, '\\') != NULL || strstr(path, "/.") != NULL;
#ifdef _WIN32
    forbidden = forbidden || strchr(path, ':') != NULL;  // Drive letters and alternate data streams
#endif
    if (forbidden) {
        return static_error(sock, 403, keep_alive, head_only, status);
    }

    char fs_path[MAX_PATH_LEN * 2];
    char resolved[MAX_PATH_LEN];
    struct stat st;
    snprintf(fs_path, sizeof(fs_path), "%s%s", server_root, path);
    file_id_t file_id;
    if (resolve_existing(fs_path, resolved, sizeof(resolved), &st, &file_id) < 0) {
        return static_error(sock, 404, keep_alive, head_only, status);
    }

    if (S_ISDIR(st.st_mode)) {
        // "/docs" redirects to "/docs/" so relative links in its index.html resolve correctly
        size_t raw_len = strlen(raw_path);
        if (raw_path[raw_len - 1] != '/') {
            const char* location_path = raw_path;
            while (location_path[0] == '/' && location_path[1] == '/') location_path++;  // No "//host" redirects
            char location[MAX_PATH_LEN + 32];
            snprintf(location, sizeof(location), "Location: %s/%s%s\r\n", location_path,
                     query ? "?" : "", query ? query + 1 : "");
            static const char body[] = "301 Moved Permanently\n";
            *status = 301;
            return send_response(sock, 301, "Moved Permanently", "text/plain; charset=utf-8",
                                 body, sizeof(body) - 1, keep_alive, head_only, location) == 0 && keep_alive;
        }
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", resolved);
        char dir_resolved[MAX_PATH_LEN];
        snprintf(dir_resolved, sizeof(dir_resolved), "%s", resolved);
        if (resolve_existing(fs_path, resolved, sizeof(resolved), &st, &file_id) < 0) {
            // No index.html: list the folder instead, if allowed and inside the served directory
            if (!config.enable_directory_listing) {
                return static_error(sock, 404, keep_alive, head_only, status);
            }
            if (!path_is_within(dir_resolved, server_root) || has_hidden_segment(dir_resolved, server_root)) {
                return static_error(sock, 403, keep_alive, head_only, status);
            }
            *status = 200;
            return send_directory_listing(sock, dir_resolved, path, keep_alive, head_only) == 0 && keep_alive;
        }
    }

    // The real path (symlinks and junctions resolved) must stay inside the served directory,
    // must not be hidden under another name, must be a regular file, and must not be the
    // server's own config file
    if (!path_is_within(resolved, server_root) || has_hidden_segment(resolved, server_root)) {
        return static_error(sock, 403, keep_alive, head_only, status);
    }
    int is_config_file = have_config_id && file_id.volume == config_id.volume && file_id.index == config_id.index;
    if (!S_ISREG(st.st_mode) || is_config_file) {
        return static_error(sock, 404, keep_alive, head_only, status);
    }

    // Honor a Range header unless If-Range is present (we send no validators, so it can't match)
    char range_value[128];
    char if_range[8];
    const char* range = NULL;
    if (find_header(req->headers, req->headers_len, "Range", range_value, sizeof(range_value)) &&
        !find_header(req->headers, req->headers_len, "If-Range", if_range, sizeof(if_range))) {
        range = range_value;
    }

    if (serve_file(sock, resolved, &st, range, keep_alive, head_only, status) < 0) return 0;
    if (config.enable_statistics && (*status == 200 || *status == 206)) ATOMIC_INC(successful_requests);
    return keep_alive;
}

// ========================================
// CONNECTION HANDLING
// ========================================

// Print one line per request, e.g. "127.0.0.1 GET /app.js 200 0.4 ms".
// Control characters in the target are replaced so a request can't inject terminal escapes.
static void log_request(const char* client_ip, const char* method, const char* target, int status,
                        double elapsed_ms) {
    char safe_target[128];
    size_t i = 0;
    for (; target[i] && i < sizeof(safe_target) - 4; i++) {
        unsigned char c = (unsigned char)target[i];
        safe_target[i] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    if (target[i]) {
        memcpy(safe_target + i, "...", 3);
        i += 3;
    }
    safe_target[i] = '\0';
    if (status > 0) {
        printf("%s %s %s %d %.1f ms\n", client_ip, method, safe_target, status, elapsed_ms);
    } else {
        printf("%s %s %s (client disconnected) %.1f ms\n", client_ip, method, safe_target, elapsed_ms);
    }
    fflush(stdout);
}

static void get_client_ip(int sock, char* out, size_t out_size) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    snprintf(out, out_size, "unknown");
    if (getpeername(sock, (struct sockaddr*)&addr, &len) != 0) return;
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, out, (socklen_t)out_size);
    } else if (addr.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&addr)->sin6_addr, out, (socklen_t)out_size);
    }
}

// Read and throw away the rest of a request body, within the request deadline.
// Returns 0 on success.
static int discard_body(int sock, long remaining) {
    char chunk[IO_CHUNK_SIZE];
    double deadline = now_ms() + request_deadline_seconds() * 1000.0;
    while (remaining > 0) {
        double left = deadline - now_ms();
        if (left <= 0) return -1;
        set_socket_timeouts(sock, (int)(left / 1000.0) + 1);
        int want = remaining > (long)sizeof(chunk) ? (int)sizeof(chunk) : (int)remaining;
        ssize_t n = recv(sock, chunk, want, 0);
        if (n <= 0) return -1;
        remaining -= (long)n;
    }
    set_socket_timeouts(sock, KEEP_ALIVE_TIMEOUT);
    return 0;
}

// Serve requests on one connection until the client closes it, it sits idle for
// keep_alive_timeout seconds, or it reaches MAX_REQUESTS_PER_CONNECTION.
static void handle_connection(int client_socket) {
    size_t capacity = (size_t)BUFFER_SIZE;
    size_t used = 0;
    char* buffer = malloc(capacity);
    if (!buffer) {
        close(client_socket);
        return;
    }

    set_socket_timeouts(client_socket, KEEP_ALIVE_TIMEOUT);
    char client_ip[INET6_ADDRSTRLEN];
    get_client_ip(client_socket, client_ip, sizeof(client_ip));

    for (int request_count = 1; request_count <= MAX_REQUESTS_PER_CONNECTION; request_count++) {
        // Read until the whole header block has arrived (it may already be buffered).
        // Waiting for the first byte uses keep_alive_timeout; once a request has started,
        // the rest of its headers must arrive within the request deadline.
        size_t header_len;
        double header_deadline = 0;
        while ((header_len = find_header_end(buffer, used)) == 0) {
            if (used == capacity) {
                send_error(client_socket, 431, 0, 0);
                goto done;
            }
            if (used > 0) {
                if (header_deadline == 0) header_deadline = now_ms() + request_deadline_seconds() * 1000.0;
                double left = header_deadline - now_ms();
                if (left <= 0) {
                    send_error(client_socket, 408, 0, 0);
                    goto done;
                }
                set_socket_timeouts(client_socket, (int)(left / 1000.0) + 1);
            }
            ssize_t n = recv(client_socket, buffer + used, (int)(capacity - used), 0);
            if (n <= 0) {
                if (n < 0 && used > 0 && socket_timed_out()) send_error(client_socket, 408, 0, 0);
                goto done;  // Client closed, idle timeout, or error
            }
            used += (size_t)n;
        }
        if (header_deadline != 0) set_socket_timeouts(client_socket, KEEP_ALIVE_TIMEOUT);

        http_request_t req;
        double started = now_ms();
        int error_status = parse_request(buffer, header_len, &req);
        if (error_status) {
            send_error(client_socket, error_status, 0, 0);
            if (config.enable_request_log) printf("%s (malformed request) %d\n", client_ip, error_status);
            break;
        }
        if (config.enable_statistics) ATOMIC_INC(total_requests);
        if (req.has_transfer_encoding) {
            send_error(client_socket, 501, 0, 0);  // Chunked request bodies are not supported
            if (config.enable_request_log) log_request(client_ip, req.method, req.target, 501, now_ms() - started);
            break;
        }

        int keep_alive = req.keep_alive && request_count < MAX_REQUESTS_PER_CONNECTION;
        int status = 0;

        // Part of the body may already be buffered; anything after it is the next request
        size_t body_in_buf = used - header_len;
        if ((long)body_in_buf > req.content_length) body_in_buf = (size_t)req.content_length;
        long body_remaining = req.content_length - (long)body_in_buf;

        size_t prefix_len = 0;
        proxy_target_t* target = config.enable_reverse_proxy ? find_proxy_target(req.target, &prefix_len) : NULL;
        if (target) {
            keep_alive = proxy_request(client_socket, &req, buffer + header_len, body_in_buf,
                                       target, prefix_len, client_ip, keep_alive, &status);
        } else {
            // Static files ignore request bodies; read small ones so the connection can be reused
            if (body_remaining > MAX_DISCARDED_BODY) {
                keep_alive = 0;
            } else if (body_remaining > 0) {
                // A client that sent "Expect: 100-continue" holds the body back until told to go ahead
                static const char continue_response[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (body_in_buf == 0 && req.expect_continue &&
                    send_all(client_socket, continue_response, sizeof(continue_response) - 1) < 0) break;
                if (discard_body(client_socket, body_remaining) < 0) break;
            }
            keep_alive = serve_static(client_socket, &req, keep_alive, &status);
        }
        if (config.enable_request_log) log_request(client_ip, req.method, req.target, status, now_ms() - started);
        if (!keep_alive) break;

        // Keep any pipelined bytes that follow this request
        size_t consumed = header_len + body_in_buf;
        memmove(buffer, buffer + consumed, used - consumed);
        used -= consumed;
    }

done:
    free(buffer);
    close(client_socket);
}

// ========================================
// PER-IP CONNECTION LIMIT
// ========================================
//
// Counts open connections per client address so one client can't take every thread.

typedef struct ip_count {
    uint32_t ip;
    int count;
    struct ip_count* next;
} ip_count_t;

#define IP_BUCKETS 256
static ip_count_t* ip_buckets[IP_BUCKETS];
#ifdef _WIN32
static CRITICAL_SECTION ip_lock;
#define IP_LOCK() EnterCriticalSection(&ip_lock)
#define IP_UNLOCK() LeaveCriticalSection(&ip_lock)
#else
static pthread_mutex_t ip_lock = PTHREAD_MUTEX_INITIALIZER;
#define IP_LOCK() pthread_mutex_lock(&ip_lock)
#define IP_UNLOCK() pthread_mutex_unlock(&ip_lock)
#endif

static ip_count_t** ip_bucket(uint32_t ip) {
    return &ip_buckets[(ip * 2654435761u) >> 24];
}

// Count a new connection from 'ip'. Returns 0 if that address already has
// max_connections_per_ip connections open, meaning the new one should be refused.
static int ip_acquire(uint32_t ip) {
    if (config.max_connections_per_ip == 0) return 1;
    int allowed = 1;
    IP_LOCK();
    ip_count_t** bucket = ip_bucket(ip);
    ip_count_t* entry = *bucket;
    while (entry && entry->ip != ip) entry = entry->next;
    if (!entry) {
        entry = malloc(sizeof(*entry));
        if (entry) {
            entry->ip = ip;
            entry->count = 1;
            entry->next = *bucket;
            *bucket = entry;
        }
    } else if (entry->count >= config.max_connections_per_ip) {
        allowed = 0;
    } else {
        entry->count++;
    }
    IP_UNLOCK();
    return allowed;
}

// A connection from 'ip' has closed
static void ip_release(uint32_t ip) {
    if (config.max_connections_per_ip == 0) return;
    IP_LOCK();
    for (ip_count_t** link = ip_bucket(ip); *link; link = &(*link)->next) {
        if ((*link)->ip == ip) {
            if (--(*link)->count == 0) {
                ip_count_t* done = *link;
                *link = done->next;
                free(done);
            }
            break;
        }
    }
    IP_UNLOCK();
}

// What the accept loop hands to each connection thread
typedef struct {
    int sock;
    uint32_t ip;
} connection_arg_t;

static void run_connection(void* arg) {
    connection_arg_t conn = *(connection_arg_t*)arg;
    free(arg);
    handle_connection(conn.sock);
    ip_release(conn.ip);
    ATOMIC_DEC(active_connection_threads);
}

// Per-connection thread so a keep-alive client doesn't block everyone else
#ifdef _WIN32
static unsigned __stdcall connection_thread(void* arg) {
    run_connection(arg);
    return 0;
}
#else
static void* connection_thread(void* arg) {
    run_connection(arg);
    return NULL;
}
#endif

// ========================================
// SIGNAL HANDLING & USAGE
// ========================================

// Exit on Ctrl+C / SIGTERM. Only async-signal-safe calls are used on POSIX.
static void signal_handler(int sig) {
    (void)sig;
    static const char message[] = "\nShutting down server...\n";
#ifdef _WIN32
    fputs(message, stdout);
    fflush(stdout);
#else
    ssize_t ignored = write(STDOUT_FILENO, message, sizeof(message) - 1);
    (void)ignored;
#endif
    _exit(0);
}

static void print_usage(const char* program_name) {
    printf("WIP - Web In a Pinch v%s\n", WIP_VERSION);
    printf("A small single-file HTTP server with an optional reverse proxy\n\n");
    printf("Usage: %s [host] [port] [directory] [options]\n\n", program_name);
    printf("Arguments:\n");
    printf("  host       IP address to bind to (default: 127.0.0.1, use 0.0.0.0 for all interfaces)\n");
    printf("  port       Port number to listen on (default: 8080)\n");
    printf("  directory  Directory to serve files from (default: current directory)\n");
    printf("\nOptions:\n");
    printf("  --config FILE        Load settings from FILE (default: wip.json)\n");
    printf("  --create-config      Write a default config file and exit\n");
    printf("  --proxy PREFIX=URL   Forward requests under PREFIX to an HTTP server (repeatable)\n");
    printf("  -q, --quiet          Don't print a line for each request\n");
    printf("  -h, --help           Show this help\n");
    printf("\nProxy routes:\n");
    printf("  --proxy /api=http://localhost:3000       /api/users -> http://localhost:3000/users\n");
    printf("  --proxy /api=http://localhost:3000/api   /api/users -> http://localhost:3000/api/users\n");
    printf("  Routes can also be listed in the \"proxy\" section of the config file.\n");
    printf("\nSpecial endpoints:\n");
    printf("  /__stats   Server statistics (JSON)\n");
    printf("\nExamples:\n");
    printf("  %s                                    # Serve this directory on 127.0.0.1:8080\n", program_name);
    printf("  %s 0.0.0.0 3000 ./public              # Serve ./public to your network on port 3000\n", program_name);
    printf("  %s --proxy /api=http://127.0.0.1:3001  # Static files plus an API backend\n", program_name);
}

// ========================================
// MAIN FUNCTION
// ========================================

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 8080;
    const char* directory = ".";
    int create_config = 0;

    // First pass: options that decide which config file to use
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --config needs a file name\n");
                return 1;
            }
            snprintf(config.config_file, sizeof(config.config_file), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--create-config") == 0) {
            create_config = 1;
        }
    }
    if (create_config) {
        return create_default_config(config.config_file) == 0 ? 0 : 1;
    }

    // Load the config file first so command-line options can override it
    if (load_config(config.config_file) < 0) {
        fprintf(stderr, "Fix the errors in '%s' and try again\n", config.config_file);
        return 1;
    }

    // Second pass: positional arguments and proxy routes
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            i++;
        } else if (strcmp(argv[i], "--create-config") == 0) {
            continue;
        } else if (strcmp(argv[i], "--proxy") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --proxy needs PREFIX=URL\n");
                return 1;
            }
            char spec[1024];
            snprintf(spec, sizeof(spec), "%s", argv[++i]);
            char* equals = strchr(spec, '=');
            if (!equals) {
                fprintf(stderr, "Error: --proxy expects PREFIX=URL, got '%s'\n", spec);
                return 1;
            }
            *equals = '\0';
            if (add_proxy_target(spec, equals + 1) < 0) return 1;
            config.enable_reverse_proxy = 1;  // Asking for a route on the command line turns the proxy on
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            config.enable_request_log = 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s (see --help)\n", argv[i]);
            return 1;
        } else if (positional == 0) {
            host = argv[i];
            positional++;
        } else if (positional == 1) {
            char* end;
            long p = strtol(argv[i], &end, 10);
            if (*end != '\0' || p < 1 || p > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)p;
            positional++;
        } else if (positional == 2) {
            directory = argv[i];
            positional++;
        } else {
            fprintf(stderr, "Too many arguments (see --help)\n");
            return 1;
        }
    }
    if (strcmp(host, "localhost") == 0) host = "127.0.0.1";

    // Resolve the served directory; the config file itself is never served
    struct stat root_stat;
    if (resolve_existing(directory, server_root, sizeof(server_root), &root_stat, NULL) < 0 ||
        !S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a directory\n", directory);
        return 1;
    }
    struct stat config_stat;
    char config_path[MAX_PATH_LEN];
    have_config_id = resolve_existing(config.config_file, config_path, sizeof(config_path),
                                      &config_stat, &config_id) == 0;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    server_start_time = time(NULL);
    if (config.enable_response_caching) {
        init_cache();
    }
    const char* event_backend = NULL;
    if (config.enable_event_driven) {
        event_backend = init_event_backend();
        if (!event_backend) fprintf(stderr, "Warning: no event backend available on this platform\n");
    }

    signal(SIGINT, signal_handler);
#ifndef _WIN32
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // A client disconnecting mid-response must not kill the server
#endif

    server_socket = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return 1;
    }

#ifdef _WIN32
    // On Windows SO_REUSEADDR would let another process take over the port; ask for exclusive use instead
    int exclusive = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive));
#else
    int reuse = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(server_socket);
        return 1;
    }
#endif

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid host address: %s\n", host);
        close(server_socket);
        return 1;
    }
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        return 1;
    }
    if (listen(server_socket, 1024) < 0) {
        perror("listen");
        close(server_socket);
        return 1;
    }

    printf("WIP (Web In a Pinch) v%s\n", WIP_VERSION);
    printf("Server running at http://%s:%d\n", host, port);
    printf("Serving files from: %s\n", server_root);
    if (strcmp(host, "0.0.0.0") == 0) {
        printf("Note: listening on all interfaces - other machines on your network can reach this server\n");
    }
    if (proxy_target_count > 0) {
        printf("Reverse proxy: %d route(s)%s\n", proxy_target_count,
               config.enable_reverse_proxy ? "" : " (disabled by enable_reverse_proxy: false)");
    }
    if (event_backend) {
        printf("Event backend: %s (experimental; requests are still handled one thread per connection)\n", event_backend);
    }
    printf("Press Ctrl+C to stop\n\n");
    fflush(stdout);

#ifndef _WIN32
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    pthread_attr_setstacksize(&thread_attr, THREAD_STACK_SIZE);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
#else
    InitializeCriticalSection(&ip_lock);
#endif

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = (int)accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAEINTR) continue;
            if (err == WSAEMFILE || err == WSAENOBUFS) {
                Sleep(100);  // Out of sockets or memory: wait for connections to finish instead of exiting
                continue;
            }
            fprintf(stderr, "accept failed (error %d)\n", err);
#else
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                struct timespec pause = { 0, 100 * 1000 * 1000 };
                nanosleep(&pause, NULL);  // Out of file descriptors or memory: wait instead of exiting
                continue;
            }
            perror("accept");
#endif
            break;
        }

        // Enforce max_connections to bound thread creation
        if (ATOMIC_INC(active_connection_threads) > MAX_CONNECTIONS) {
            ATOMIC_DEC(active_connection_threads);
            close(client_socket);
            continue;
        }

        // ...and max_connections_per_ip so one client can't take them all
        uint32_t client_ip = client_addr.sin_addr.s_addr;
        if (!ip_acquire(client_ip)) {
            ATOMIC_DEC(active_connection_threads);
            close(client_socket);
            continue;
        }
        connection_arg_t* conn = malloc(sizeof(*conn));
        if (!conn) {
            ip_release(client_ip);
            ATOMIC_DEC(active_connection_threads);
            close(client_socket);
            continue;
        }
        conn->sock = client_socket;
        conn->ip = client_ip;

        // Handle each connection on its own thread (required for keep-alive). If no thread
        // can be created, drop the connection rather than block the accept loop on it.
#ifdef _WIN32
        HANDLE thread = (HANDLE)_beginthreadex(NULL, THREAD_STACK_SIZE, connection_thread, conn,
                                               STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (thread) {
            CloseHandle(thread);
        } else {
            free(conn);
            ip_release(client_ip);
            ATOMIC_DEC(active_connection_threads);
            close(client_socket);
        }
#else
        pthread_t thread;
        if (pthread_create(&thread, &thread_attr, connection_thread, conn) != 0) {
            free(conn);
            ip_release(client_ip);
            ATOMIC_DEC(active_connection_threads);
            close(client_socket);
        }
#endif
    }

    close(server_socket);
    if (config.enable_response_caching) {
        cleanup_cache();
    }
    cleanup_event_backend();
#ifdef _WIN32
    WSACleanup();
#endif
    printf("Server stopped\n");
    return 1;
}
