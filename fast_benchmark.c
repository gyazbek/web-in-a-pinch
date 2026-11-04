/*
 * fast_benchmark - a small HTTP load generator for testing wip
 *
 * Opens N keep-alive connections, each on its own thread, and sends GET
 * requests until the total is reached, then reports throughput and latency.
 *
 * Usage: fast_benchmark <host> <port> <total_requests> <connections> [path]
 */

#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
#else
    #define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <pthread.h>
    #include <time.h>
#endif

#define MAX_CONNECTIONS 1000
#define BUFFER_SIZE 65536       // Response headers must fit in this; bodies are streamed through it
#define RECV_TIMEOUT_SECONDS 10

typedef struct {
    int requests;               // Requests this connection should send
    int ok;                     // 2xx responses
    int failed;                 // Errors and non-2xx responses
    int bad_status;             // Last non-2xx status seen, for the report
    int connections_opened;
    double* latencies;          // Seconds per successful request
} worker_t;

static struct addrinfo* target = NULL;
static char request[2048];
static int request_len = 0;

// ---- Wall-clock timer ----

#ifdef _WIN32
static double timer_frequency = 0;
#endif

static void init_timer(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    timer_frequency = (double)frequency.QuadPart;
#endif
}

static double now_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / timer_frequency;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

// ---- Networking ----

// Connect to the first address that accepts (e.g. "localhost" may resolve to ::1 and 127.0.0.1)
static int open_connection(void) {
    int sock = -1;
    for (struct addrinfo* ai = target; ai; ai = ai->ai_next) {
        sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    if (sock < 0) return -1;
#ifdef _WIN32
    DWORD timeout_ms = RECV_TIMEOUT_SECONDS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = { RECV_TIMEOUT_SECONDS, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return sock;
}

static int send_all(int sock, const char* data, int len) {
    while (len > 0) {
        int sent = (int)send(sock, data, len, 0);
        if (sent <= 0) return -1;
        data += sent;
        len -= sent;
    }
    return 0;
}

static int starts_with_ci(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    }
    return 1;
}

// Find a header (case-insensitive name) in [headers, end). Returns a pointer to its value or NULL.
static const char* find_header(const char* headers, const char* end, const char* name) {
    size_t name_len = strlen(name);
    const char* line = headers;
    while (line < end) {
        const char* eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol) break;
        if ((size_t)(eol - line) > name_len && line[name_len] == ':' && starts_with_ci(line, name)) {
            const char* value = line + name_len + 1;
            while (*value == ' ' || *value == '\t') value++;
            return value;
        }
        line = eol + 1;
    }
    return NULL;
}

// Send one request and read the whole response. Returns the HTTP status, or -1 on error.
// Sets *keep_open to 1 if the connection can be used for the next request.
static int do_request(int sock, char* buf, int* keep_open) {
    *keep_open = 0;
    if (send_all(sock, request, request_len) < 0) return -1;

    // Read until the end of the header block
    size_t used = 0;
    char* header_end = NULL;
    while (!header_end) {
        if (used == BUFFER_SIZE - 1) return -1;
        int n = (int)recv(sock, buf + used, (int)(BUFFER_SIZE - 1 - used), 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
    }
    if (used < 12 || strncmp(buf, "HTTP/1.", 7) != 0) return -1;
    int status = atoi(buf + 9);

    size_t header_len = (size_t)(header_end - buf) + 4;
    const char* content_length = find_header(buf, header_end + 2, "Content-Length");
    const char* connection = find_header(buf, header_end + 2, "Connection");
    int server_closes = connection && starts_with_ci(connection, "close");

    if (!content_length) {
        // No Content-Length: the body ends when the server closes the connection
        while (recv(sock, buf, BUFFER_SIZE, 0) > 0) {}
        return status;
    }

    // Read (and discard) the rest of the body
    long remaining = strtol(content_length, NULL, 10) - (long)(used - header_len);
    while (remaining > 0) {
        int want = remaining > BUFFER_SIZE ? BUFFER_SIZE : (int)remaining;
        int n = (int)recv(sock, buf, want, 0);
        if (n <= 0) return -1;
        remaining -= n;
    }
    *keep_open = !server_closes && remaining == 0;
    return status;
}

static void run_worker(worker_t* w) {
    char* buf = malloc(BUFFER_SIZE);
    int sock = -1;
    if (!buf) {
        w->failed = w->requests;
        return;
    }
    for (int i = 0; i < w->requests; i++) {
        if (sock < 0) {
            sock = open_connection();
            if (sock < 0) {
                w->failed++;
                continue;
            }
            w->connections_opened++;
        }

        int keep_open;
        double start = now_seconds();
        int status = do_request(sock, buf, &keep_open);
        double elapsed = now_seconds() - start;

        if (status >= 200 && status < 300) {
            w->latencies[w->ok++] = elapsed;
        } else {
            w->failed++;
            if (status > 0) w->bad_status = status;
        }
        if (!keep_open) {
            close(sock);
            sock = -1;
        }
    }
    if (sock >= 0) close(sock);
    free(buf);
}

#ifdef _WIN32
static DWORD WINAPI worker_thread(LPVOID arg) {
    run_worker((worker_t*)arg);
    return 0;
}
#else
static void* worker_thread(void* arg) {
    run_worker((worker_t*)arg);
    return NULL;
}
#endif

static int compare_doubles(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

// ---- Main ----

int main(int argc, char* argv[]) {
    if (argc < 5 || argc > 6) {
        printf("Usage: %s <host> <port> <total_requests> <connections> [path]\n", argv[0]);
        printf("Example: %s 127.0.0.1 8080 100000 50 /index.html\n", argv[0]);
        printf("The path defaults to /__stats.\n");
        return 1;
    }

    const char* host = argv[1];
    const char* port = argv[2];
    long total = strtol(argv[3], NULL, 10);
    int connections = atoi(argv[4]);
    const char* path = argc == 6 ? argv[5] : "/__stats";

    if (total < 1 || total > 100000000L) {
        fprintf(stderr, "total_requests must be between 1 and 100000000\n");
        return 1;
    }
    if (connections < 1 || connections > MAX_CONNECTIONS) {
        fprintf(stderr, "connections must be between 1 and %d\n", MAX_CONNECTIONS);
        return 1;
    }
    if (connections > total) connections = (int)total;
    if (path[0] != '/') {
        fprintf(stderr, "path must start with '/'\n");
        return 1;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &target) != 0 || !target) {
        fprintf(stderr, "Cannot resolve %s:%s\n", host, port);
        return 1;
    }

    // Find the address that accepts connections once, so workers don't retry dead ones
    struct addrinfo* all_addresses = target;
    for (struct addrinfo* ai = all_addresses; ai; ai = ai->ai_next) {
        int probe = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (probe < 0) continue;
        int connected = connect(probe, ai->ai_addr, ai->ai_addrlen) == 0;
        close(probe);
        if (connected) {
            target = ai;
            break;
        }
    }

    int ipv6 = strchr(host, ':') != NULL;
    request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s%s%s:%s\r\n"
        "User-Agent: fast_benchmark\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path, ipv6 ? "[" : "", host, ipv6 ? "]" : "", port);
    if (request_len < 0 || request_len >= (int)sizeof(request)) {
        fprintf(stderr, "Path or host too long\n");
        return 1;
    }

    worker_t* workers = calloc((size_t)connections, sizeof(worker_t));
    if (!workers) return 1;
    for (int i = 0; i < connections; i++) {
        workers[i].requests = (int)(total / connections + (i < total % connections ? 1 : 0));
        workers[i].latencies = malloc((size_t)workers[i].requests * sizeof(double));
        if (!workers[i].latencies) return 1;
    }

    printf("GET http://%s%s%s:%s%s, %ld requests over %d connections\n",
           ipv6 ? "[" : "", host, ipv6 ? "]" : "", port, path, total, connections);

    init_timer();
    double start = now_seconds();

#ifdef _WIN32
    HANDLE* threads = calloc((size_t)connections, sizeof(HANDLE));
    for (int i = 0; i < connections; i++) {
        threads[i] = CreateThread(NULL, 0, worker_thread, &workers[i], 0, NULL);
        if (!threads[i]) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }
    // WaitForMultipleObjects is limited to 64 handles, so wait one at a time
    for (int i = 0; i < connections; i++) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
#else
    pthread_t* threads = calloc((size_t)connections, sizeof(pthread_t));
    for (int i = 0; i < connections; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, &workers[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }
    for (int i = 0; i < connections; i++) {
        pthread_join(threads[i], NULL);
    }
#endif

    double elapsed = now_seconds() - start;

    // Gather results
    long ok = 0, failed = 0, opened = 0;
    int bad_status = 0;
    for (int i = 0; i < connections; i++) {
        ok += workers[i].ok;
        failed += workers[i].failed;
        opened += workers[i].connections_opened;
        if (workers[i].bad_status) bad_status = workers[i].bad_status;
    }
    double* all = malloc((size_t)(ok > 0 ? ok : 1) * sizeof(double));
    if (!all) return 1;
    long k = 0;
    double sum = 0;
    for (int i = 0; i < connections; i++) {
        for (int j = 0; j < workers[i].ok; j++) {
            all[k++] = workers[i].latencies[j];
            sum += workers[i].latencies[j];
        }
    }
    qsort(all, (size_t)ok, sizeof(double), compare_doubles);

    printf("%ld ok, %ld failed in %.3f s\n", ok, failed, elapsed);
    if (bad_status) printf("Some responses had status %d\n", bad_status);
    if (ok > 0) {
        printf("%.0f requests/s\n", (double)ok / elapsed);
        printf("Latency: avg %.3f ms, p50 %.3f ms, p99 %.3f ms, max %.3f ms\n",
               sum / (double)ok * 1000.0,
               all[(size_t)((double)(ok - 1) * 0.50)] * 1000.0,
               all[(size_t)((double)(ok - 1) * 0.99)] * 1000.0,
               all[ok - 1] * 1000.0);
    }
    printf("Connections opened: %ld\n", opened);

    free(all);
    for (int i = 0; i < connections; i++) free(workers[i].latencies);
    free(workers);
    free(threads);
    freeaddrinfo(all_addresses);
#ifdef _WIN32
    WSACleanup();
#endif
    return failed ? 1 : 0;
}
