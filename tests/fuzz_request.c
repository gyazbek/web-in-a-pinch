// libFuzzer harness: hands arbitrary bytes to wip as one client connection.
//
// Everything a connection does runs for real: request parsing, keep-alive and pipelining,
// path resolution against a small test folder, ranges, listings, the cache, and the
// reverse proxy (to a stand-in backend in this process). If the input contains the
// separator below, the bytes after it are what the backend replies with, so response
// handling is fuzzed too.
//
// Linux only (needs clang's libFuzzer). Build and run with tests/fuzz.sh.

#define main wip_main
#include "../wip.c"
#undef main

#define UPSTREAM_SEPARATOR "\n--upstream--\n"

static char work_dir[] = "/tmp/wip-fuzz-XXXXXX";
static pthread_mutex_t reply_lock = PTHREAD_MUTEX_INITIALIZER;
static char reply[65536];
static size_t reply_len;
static int upstream_listener;

static void write_file(const char* name, const char* content, size_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", work_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) abort();
    fwrite(content, 1, len, f);
    fclose(f);
}

// Stand-in backend: reads a request's headers, sends the current reply and closes
static void* upstream_thread(void* arg) {
    (void)arg;
    for (;;) {
        int conn = accept(upstream_listener, NULL, NULL);
        if (conn < 0) continue;
        struct timeval tv = { 2, 0 };
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[8192];
        size_t used = 0;
        while (used < sizeof(buf) && !find_header_end(buf, used)) {
            ssize_t n = recv(conn, buf + used, sizeof(buf) - used, 0);
            if (n <= 0) break;
            used += (size_t)n;
        }
        pthread_mutex_lock(&reply_lock);
        send_all(conn, reply, reply_len);
        pthread_mutex_unlock(&reply_lock);
        shutdown(conn, SHUT_WR);
        close(conn);
    }
    return NULL;
}

// Reads and throws away everything wip sends to the client
static void* drain_thread(void* arg) {
    int sock = *(int*)arg;
    char buf[65536];
    while (recv(sock, buf, sizeof(buf), 0) > 0) {}
    return NULL;
}

int LLVMFuzzerInitialize(int* argc, char*** argv) {
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);

    // A small site: files, a folder without an index, hidden files and the config file
    if (!mkdtemp(work_dir)) abort();
    char path[512];
    snprintf(path, sizeof(path), "%s/files", work_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.git", work_dir);
    mkdir(path, 0755);
    write_file("index.html", "<h1>fuzz</h1>\n", 14);
    write_file("digits.txt", "0123456789", 10);
    write_file("files/a b.txt", "a\n", 2);
    write_file("files/<tag>&.txt", "b\n", 2);
    write_file("files/.hidden", "h\n", 2);
    write_file(".git/config", "secret\n", 7);
    write_file("wip.json", "{}\n", 3);
    static char big[200000];
    memset(big, 'x', sizeof(big));
    write_file("big.bin", big, sizeof(big));

    struct stat st;
    if (resolve_existing(work_dir, server_root, sizeof(server_root), &st, NULL) < 0) abort();
    snprintf(path, sizeof(path), "%s/wip.json", work_dir);
    char config_path[MAX_PATH_LEN];
    have_config_id = resolve_existing(path, config_path, sizeof(config_path), &st, &config_id) == 0;

    config.enable_request_log = 0;
    config.enable_reverse_proxy = 1;
    config.keep_alive_timeout = 1;
    config.proxy_timeout = 1;
    init_cache();

    // Backend on a free local port, plus a route to a port nothing listens on
    upstream_listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t addr_len = sizeof(addr);
    if (bind(upstream_listener, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(upstream_listener, 16) < 0 ||
        getsockname(upstream_listener, (struct sockaddr*)&addr, &addr_len) < 0) abort();
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", ntohs(addr.sin_port));
    add_proxy_target("/api", url);
    snprintf(url, sizeof(url), "http://localhost:%d/base/", ntohs(addr.sin_port));
    add_proxy_target("/keep/*", url);
    add_proxy_target("/dead", "http://127.0.0.1:1");
    pthread_t thread;
    pthread_create(&thread, NULL, upstream_thread, NULL);
    pthread_detach(thread);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Split the input into what the client sends and what the backend replies
    const char* input = (const char*)data;
    size_t request_len = size;
    pthread_mutex_lock(&reply_lock);
    static const char default_reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    memcpy(reply, default_reply, sizeof(default_reply) - 1);
    reply_len = sizeof(default_reply) - 1;
    const char* sep = memmem(input, size, UPSTREAM_SEPARATOR, sizeof(UPSTREAM_SEPARATOR) - 1);
    if (sep) {
        request_len = (size_t)(sep - input);
        const char* r = sep + sizeof(UPSTREAM_SEPARATOR) - 1;
        reply_len = size - (size_t)(r - input);
        if (reply_len > sizeof(reply)) reply_len = sizeof(reply);
        memcpy(reply, r, reply_len);
    }
    pthread_mutex_unlock(&reply_lock);

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) abort();
    pthread_t drain;
    pthread_create(&drain, NULL, drain_thread, &pair[0]);
    send_all(pair[0], input, request_len);
    shutdown(pair[0], SHUT_WR);  // The client has said everything it will say

    handle_connection(pair[1]);  // Closes pair[1]
    pthread_join(drain, NULL);
    close(pair[0]);
    return 0;
}
