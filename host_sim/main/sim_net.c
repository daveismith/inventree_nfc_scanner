#include "sim_net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net_sync.h"

/* The firmware's limits (main/net_link.c), so that what passes here passes there: two
 * commands taken per answer, and an answer cut at 8 KB, which then does not parse. */
#define CMD_RING    2
#define RESP_MAX    8192

static net_sync_t s_ns;
static bool s_configured;
static char s_host[128];
static char s_port[8];
static char s_path[192];
static char s_token[APP_NET_TOKEN_MAX + 1];
static char s_reader[NET_SYNC_READER_MAX];
static uint32_t s_wait_s;

static char s_cmds[CMD_RING][NET_SYNC_CMD_MAX];
static int s_cmd_head, s_cmd_count;

/* One request in flight. */
static int s_fd = -1;
static char s_out[8192];
static size_t s_out_len, s_out_sent;
static char s_in[RESP_MAX];
static size_t s_in_len;
static uint32_t s_deadline;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

bool sim_net_configured(void)
{
    return getenv("SIM_SYNC_URL") != NULL;
}

void sim_net_init(void)
{
    const char *url = getenv("SIM_SYNC_URL");
    if (url == NULL || strncmp(url, "http://", 7) != 0) {
        return;
    }
    const char *rest = url + 7;
    const char *slash = strchr(rest, '/');
    const size_t hostport_len = slash ? (size_t)(slash - rest) : strlen(rest);
    char hostport[140];
    snprintf(hostport, sizeof(hostport), "%.*s", (int)hostport_len, rest);
    const char *colon = strchr(hostport, ':');
    if (colon) {
        snprintf(s_host, sizeof(s_host), "%.*s", (int)(colon - hostport), hostport);
        snprintf(s_port, sizeof(s_port), "%.*s", (int)sizeof(s_port) - 1, colon + 1);
    } else {
        snprintf(s_host, sizeof(s_host), "%.*s", (int)sizeof(s_host) - 1, hostport);
        snprintf(s_port, sizeof(s_port), "80");
    }
    if (slash && strlen(slash) + sizeof("/sync/") > sizeof(s_path)) {
        fprintf(stderr, "SIM_SYNC_URL path too long\n");
        return;
    }
    size_t n = (size_t)snprintf(s_path, sizeof(s_path), "%.*s", (int)sizeof(s_path) - 1, slash ? slash : "");
    while (n > 0 && s_path[n - 1] == '/') {
        s_path[--n] = '\0';
    }
    snprintf(s_path + n, sizeof(s_path) - n, "/sync/");

    snprintf(s_token, sizeof(s_token), "%.*s", (int)sizeof(s_token) - 1, getenv("SIM_TOKEN") ? getenv("SIM_TOKEN") : "");
    snprintf(s_reader, sizeof(s_reader), "%.*s", (int)sizeof(s_reader) - 1, getenv("SIM_READER") ? getenv("SIM_READER") : "nfc-sim000000");
    s_wait_s = getenv("SIM_WAIT_S") ? (uint32_t)atoi(getenv("SIM_WAIT_S")) : 0;
    const uint32_t poll_ms = getenv("SIM_POLL_MS") ? (uint32_t)atoi(getenv("SIM_POLL_MS")) : 1000;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    const uint32_t boot = getenv("SIM_BOOT") ? (uint32_t)atoi(getenv("SIM_BOOT")) : (uint32_t)(rand() % 1000000 + 1);
    net_sync_init(&s_ns, s_reader, getenv("SIM_FW") ? getenv("SIM_FW") : "sim", boot, poll_ms, s_wait_s);
    signal(SIGPIPE, SIG_IGN);           /* a server that hangs up mid-request is a failed call, not the end */
    s_configured = true;
    printf("NET %s:%s%s as %s (boot %u, wait %u s, poll %u ms)\n", s_host, s_port, s_path, s_reader,
           (unsigned)boot, (unsigned)s_wait_s, (unsigned)poll_ms);
    fflush(stdout);
}

void sim_net_queue(const char *line, size_t len)
{
    if (s_configured) {
        net_sync_queue(&s_ns, line, len);
    }
}

static bool on_cmd(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    (void)len;
    if (s_cmd_count == CMD_RING) {
        return false;                   /* the main loop is behind; unacknowledged, the server re-sends */
    }
    snprintf(s_cmds[(s_cmd_head + s_cmd_count) % CMD_RING], NET_SYNC_CMD_MAX, "%s", json);
    s_cmd_count++;
    return true;
}

static void finish(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

static bool begin_request(void)
{
    static char body[6144];
    const bool hold = s_wait_s > 0 && !net_sync_has_pending(&s_ns);
    const size_t n = net_sync_request(&s_ns, now_ms(), hold, body, sizeof(body));
    if (n == 0) {
        return false;
    }
    s_out_len = (size_t)snprintf(s_out, sizeof(s_out),
                                 "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\n"
                                 "%s%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                 s_path, s_host, s_port,
                                 s_token[0] ? "Authorization: Token " : "", s_token, s_token[0] ? "\r\n" : "",
                                 n, body);
    s_out_sent = 0;
    s_in_len = 0;
    s_deadline = now_ms() + (hold ? s_wait_s * 1000 : 0) + 15000;

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(s_host, s_port, &hints, &res) != 0 || res == NULL) {
        net_sync_unreachable(&s_ns, now_ms());
        return false;
    }
    s_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_fd >= 0) {
        fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(s_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (connect(s_fd, res->ai_addr, res->ai_addrlen) < 0 && errno != EINPROGRESS) {
            finish();
        }
    }
    freeaddrinfo(res);
    if (s_fd < 0) {
        net_sync_unreachable(&s_ns, now_ms());
        return false;
    }
    return true;
}

/* A header's value, or NULL. Case-insensitive name; `headers` ends at the blank line. */
static const char *header_value(const char *headers, const char *name)
{
    const size_t n = strlen(name);
    for (const char *p = headers; (p = strchr(p, '\n')) != NULL; p++) {
        if (strncasecmp(p + 1, name, n) == 0 && p[1 + n] == ':') {
            const char *v = p + 2 + n;
            while (*v == ' ') {
                v++;
            }
            return v;
        }
    }
    return NULL;
}

/* Undo chunked transfer coding in place. Returns the body's length, or -1 if malformed. */
static long dechunk(char *body, size_t len)
{
    size_t in = 0, out = 0;
    for (;;) {
        char *end;
        const unsigned long size = strtoul(body + in, &end, 16);
        const char *crlf = strstr(end, "\r\n");
        if (crlf == NULL || end == body + in || size > len) {
            return -1;                  /* no size, or one no body of this length could hold */
        }
        in = (size_t)(crlf - body) + 2;
        if (size == 0) {
            return (long)out;
        }
        if (in + size + 2 > len) {
            return -1;
        }
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
}

/* The answer, once the server has closed the connection or the body is complete. */
static void handle_response(bool cut)
{
    s_in[s_in_len] = '\0';
    int status = 0;
    char *body = strstr(s_in, "\r\n\r\n");
    if (sscanf(s_in, "HTTP/%*d.%*d %d", &status) != 1 || body == NULL) {
        net_sync_unreachable(&s_ns, now_ms());  /* half an answer is no answer */
        return;
    }
    *body = '\0';                        /* the headers end here, for header_value */
    body += 4;
    long body_len = (long)(s_in_len - (size_t)(body - s_in));
    const char *te = header_value(s_in, "Transfer-Encoding");
    const char *cl = header_value(s_in, "Content-Length");
    if (cut) {
        /* The firmware reads what fits and parses that; so does this. */
    } else if (te && strncasecmp(te, "chunked", 7) == 0) {
        body_len = dechunk(body, (size_t)body_len);
    } else if (cl) {
        const long want = atol(cl);
        body_len = want <= body_len ? want : -1;
    }
    if (body_len < 0) {
        net_sync_unreachable(&s_ns, now_ms());
        return;
    }
    net_sync_response(&s_ns, now_ms(), status, body, (size_t)body_len, on_cmd, NULL);
}

/* Whether the whole answer is in: a Content-Length reached, or a chunked body's last chunk
 * seen. Otherwise the server's close ends it. */
static bool response_complete(void)
{
    s_in[s_in_len] = '\0';
    char *body = strstr(s_in, "\r\n\r\n");
    if (body == NULL) {
        return false;
    }
    *body = '\0';                        /* headers end here for header_value; put back below */
    const char *cl = header_value(s_in, "Content-Length");
    const char *te = header_value(s_in, "Transfer-Encoding");
    bool done = false;
    if (te && strncasecmp(te, "chunked", 7) == 0) {
        done = strstr(body + 4, "\r\n0\r\n") != NULL;
    } else if (cl) {
        const long want = atol(cl);
        done = want >= 0 && s_in_len - (size_t)(body + 4 - s_in) >= (size_t)want;
    }
    *body = '\r';
    return done;
}

static void step_request(void)
{
    struct pollfd p = { .fd = s_fd, .events = s_out_sent < s_out_len ? POLLOUT : POLLIN };
    if (poll(&p, 1, 0) <= 0) {
        if ((int32_t)(now_ms() - s_deadline) > 0) {
            finish();
            net_sync_unreachable(&s_ns, now_ms());
        }
        return;
    }
    if (p.revents & (POLLERR | POLLNVAL)) {
        finish();
        net_sync_unreachable(&s_ns, now_ms());
        return;
    }
    if (s_out_sent < s_out_len) {
        const ssize_t n = send(s_fd, s_out + s_out_sent, s_out_len - s_out_sent, 0);
        if (n > 0) {
            s_out_sent += (size_t)n;
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            finish();
            net_sync_unreachable(&s_ns, now_ms());
        }
        return;
    }
    const ssize_t n = recv(s_fd, s_in + s_in_len, sizeof(s_in) - 1 - s_in_len, 0);
    if (n > 0) {
        s_in_len += (size_t)n;
        if (response_complete()) {
            finish();
            handle_response(false);
        } else if (s_in_len >= sizeof(s_in) - 1) {
            /* As the firmware does: what fits is handed on, cut, and does not parse. */
            fprintf(stderr, "sim_net: an answer over %d bytes was cut\n", RESP_MAX);
            finish();
            handle_response(true);
        }
    } else if (n == 0) {
        finish();
        handle_response(false);
    } else if (errno != EAGAIN && errno != EINTR) {
        finish();
        net_sync_unreachable(&s_ns, now_ms());
    }
}

bool sim_net_step(char *cmd_out, size_t cap)
{
    if (!s_configured) {
        return false;
    }
    if (s_fd >= 0) {
        step_request();
    } else if (net_sync_due(&s_ns, now_ms())) {
        begin_request();
    }
    if (s_cmd_count > 0) {
        snprintf(cmd_out, cap, "%s", s_cmds[s_cmd_head]);
        s_cmd_head = (s_cmd_head + 1) % CMD_RING;
        s_cmd_count--;
        return true;
    }
    return false;
}

void sim_net_status(app_net_status_t *out)
{
    static char url[sizeof(s_host) + sizeof(s_port) + sizeof(s_path) + 16];   /* room for all three, whatever they hold */
    snprintf(url, sizeof(url), "http://%s:%s%s", s_host, s_port, s_path);
    out->enabled = s_configured;
    out->wifi = "connected";
    out->ssid = "sim";
    out->ip = "127.0.0.1";
    out->url = s_configured ? url : NULL;
    out->has_token = s_token[0] != '\0';
    out->reader = s_reader;
    out->link = s_configured ? net_sync_link_state(&s_ns) : "off";
    out->last_status = s_ns.last_status;
    out->poll_ms = s_ns.poll_ms;
    out->wait_s = s_ns.wait_s;
    out->queued = s_ns.count;
    out->dropped = s_ns.dropped;
}
