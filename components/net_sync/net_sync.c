#include "net_sync.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void net_sync_init(net_sync_t *ns, const char *reader, uint32_t boot, uint32_t poll_ms, uint32_t wait_s)
{
    memset(ns, 0, sizeof(*ns));
    snprintf(ns->reader, sizeof(ns->reader), "%s", reader);
    ns->boot = boot;
    net_sync_set_pacing(ns, poll_ms, wait_s);
}

void net_sync_set_pacing(net_sync_t *ns, uint32_t poll_ms, uint32_t wait_s)
{
    ns->poll_ms = poll_ms;
    ns->wait_s = wait_s;
}

void net_sync_reconfigured(net_sync_t *ns)
{
    ns->refused = false;
    ns->backoff_ms = 0;
    ns->next_at = ns->last_started;     /* due now */
    ns->ever_called = false;
}

void net_sync_new_server(net_sync_t *ns)
{
    net_sync_reconfigured(ns);
    ns->head = 0;
    ns->count = 0;
    ns->cmd_ack = 0;
    ns->sent_through = 0;
    ns->server_poll_ms = 0;
    ns->last_status = 0;
    ns->generation++;
}

uint32_t net_sync_generation(const net_sync_t *ns)
{
    return ns->generation;
}

/* The message's name: "rsp", or the event's name. */
static bool line_kind(const char *line, size_t len, const char **name, size_t *name_len)
{
    static const char RSP[] = "{\"rsp\":\"";
    static const char EVT[] = "{\"evt\":\"";
    const char *p;
    if (len > sizeof(RSP) && strncmp(line, RSP, sizeof(RSP) - 1) == 0) {
        *name = "rsp";
        *name_len = 3;
        return true;
    }
    if (len > sizeof(EVT) && strncmp(line, EVT, sizeof(EVT) - 1) == 0) {
        p = line + sizeof(EVT) - 1;
        const char *end = memchr(p, '"', len - (size_t)(p - line));
        if (end == NULL) {
            return false;
        }
        *name = p;
        *name_len = (size_t)(end - p);
        return true;
    }
    return false;
}

static bool is(const char *name, size_t n, const char *want)
{
    return strlen(want) == n && memcmp(name, want, n) == 0;
}

static net_sync_msg_t *slot(net_sync_t *ns, uint8_t i)
{
    return &ns->queue[(ns->head + i) % NET_SYNC_QUEUE_LEN];
}

/* Make room: the oldest message that may be dropped goes, failing that the oldest of all. */
static void drop_one(net_sync_t *ns)
{
    uint8_t victim = 0;
    for (uint8_t i = 0; i < ns->count; i++) {
        if (!slot(ns, i)->keep) {
            victim = i;
            break;
        }
    }
    for (uint8_t i = victim; i + 1 < ns->count; i++) {
        *slot(ns, i) = *slot(ns, i + 1);
    }
    ns->count--;
    ns->dropped++;
}

bool net_sync_queue(net_sync_t *ns, const char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    const char *name;
    size_t name_len;
    if (!line_kind(line, len, &name, &name_len)) {
        return false;
    }
    bool keep;
    if (is(name, name_len, "rsp") || is(name, name_len, "waiting") || is(name, name_len, "writing")
            || is(name, name_len, "done") || is(name, name_len, "failed") || is(name, name_len, "ota")
            || is(name, name_len, "error")) {
        keep = true;                    /* `error`: a command of the server's that could not be read */
    } else if (is(name, name_len, "tag")) {
        keep = false;
    } else {
        return false;                   /* hello, tag_removed, log, net: not for the server */
    }

    char seq_field[24];
    const int seq_len = snprintf(seq_field, sizeof(seq_field), "{\"seq\":%lu,", (unsigned long)(ns->last_seq + 1));
    if ((size_t)seq_len + len - 1 >= NET_SYNC_MSG_MAX) {
        ns->dropped++;
        return false;                   /* longer than a slot: a `failed` carrying a huge uri */
    }
    if (ns->count == NET_SYNC_QUEUE_LEN) {
        drop_one(ns);
    }
    net_sync_msg_t *m = slot(ns, ns->count);
    m->seq = ++ns->last_seq;
    m->keep = keep;
    memcpy(m->json, seq_field, (size_t)seq_len);
    memcpy(m->json + seq_len, line + 1, len - 1);   /* past the '{' */
    m->len = (uint16_t)((size_t)seq_len + len - 1);
    ns->count++;
    return true;
}

bool net_sync_has_pending(const net_sync_t *ns)
{
    return ns->count > 0;
}

uint32_t net_sync_delay_ms(const net_sync_t *ns, uint32_t now_ms)
{
    if (!ns->ever_called) {
        return 0;
    }
    const int32_t until = (int32_t)(ns->next_at - now_ms);
    if (ns->refused || ns->backoff_ms) {
        return until > 0 ? (uint32_t)until : 0;
    }
    if (ns->count > 0 && ns->last_seq > ns->sent_through) {
        return 0;                       /* something new to report: no waiting */
    }
    /* Messages the server has already seen and not acknowledged go again at the idle pace,
     * never in a tight loop. */
    return until > 0 ? (uint32_t)until : 0;
}

bool net_sync_due(const net_sync_t *ns, uint32_t now_ms)
{
    return net_sync_delay_ms(ns, now_ms) == 0;
}

static bool put(char *out, size_t cap, size_t *at, const char *s, size_t n)
{
    if (*at + n >= cap) {
        return false;
    }
    memcpy(out + *at, s, n);
    *at += n;
    out[*at] = '\0';
    return true;
}

size_t net_sync_request(net_sync_t *ns, uint32_t now_ms, bool hold, char *out, size_t cap)
{
    char head[160];
    const int n = snprintf(head, sizeof(head),
                           "{\"reader\":\"%s\",\"boot\":%lu,\"proto\":%d,\"ack\":%lu,\"wait_s\":%lu,\"msgs\":[",
                           ns->reader, (unsigned long)ns->boot, NET_SYNC_PROTO, (unsigned long)ns->cmd_ack,
                           (unsigned long)(hold ? ns->wait_s : 0));
    size_t at = 0;
    if (n < 0 || !put(out, cap, &at, head, (size_t)n)) {
        return 0;
    }
    /* As many of the queued messages as fit, oldest first; what does not goes next time. One
     * that could never fit on its own is dropped and counted, so that it cannot block the
     * rest for ever. */
    uint32_t carried = ns->sent_through;
    for (uint8_t i = 0; i < ns->count; i++) {
        const net_sync_msg_t *m = slot(ns, i);
        const size_t need = (i > 0 ? 1 : 0) + m->len + 2;      /* comma, message, "]}" */
        if (at + need >= cap) {
            if (i == 0) {
                ns->head = (uint8_t)((ns->head + 1) % NET_SYNC_QUEUE_LEN);
                ns->count--;
                ns->dropped++;
                i--;
                continue;
            }
            break;
        }
        if (i > 0) {
            put(out, cap, &at, ",", 1);
        }
        put(out, cap, &at, m->json, m->len);
        if (m->seq > carried) {
            carried = m->seq;
        }
    }
    if (!put(out, cap, &at, "]}", 2)) {
        return 0;
    }
    ns->last_started = now_ms;
    ns->sent_through = carried;
    ns->ever_called = true;
    return at;
}

static void schedule_idle(net_sync_t *ns)
{
    const uint32_t interval = ns->server_poll_ms > ns->poll_ms ? ns->server_poll_ms : ns->poll_ms;
    ns->next_at = ns->last_started + interval;
}

static void back_off(net_sync_t *ns, uint32_t now_ms)
{
    ns->backoff_ms = ns->backoff_ms == 0 ? NET_SYNC_BACKOFF_MIN_MS
                     : (ns->backoff_ms * 2 > NET_SYNC_BACKOFF_MAX_MS ? NET_SYNC_BACKOFF_MAX_MS : ns->backoff_ms * 2);
    ns->next_at = now_ms + ns->backoff_ms;
}

void net_sync_unreachable(net_sync_t *ns, uint32_t now_ms)
{
    ns->last_status = -1;
    back_off(ns, now_ms);
}

/* A JSON number as a sequence number: whole, and within what the counters hold. */
static bool as_seq(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 2147483647.0
            || item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static void acknowledged(net_sync_t *ns, uint32_t ack)
{
    while (ns->count > 0 && slot(ns, 0)->seq <= ack) {
        ns->head = (uint8_t)((ns->head + 1) % NET_SYNC_QUEUE_LEN);
        ns->count--;
    }
}

void net_sync_response(net_sync_t *ns, uint32_t now_ms, int status, const char *body, size_t len,
                       net_sync_cmd_fn on_cmd, void *ctx)
{
    ns->last_status = status;
    if (status == 401 || status == 403 || status == 404) {
        ns->refused = true;
        ns->backoff_ms = 0;
        ns->next_at = now_ms + NET_SYNC_REFUSED_RETRY_MS;
        return;
    }
    if (status < 200 || status >= 300) {
        back_off(ns, now_ms);
        return;
    }

    cJSON *obj = cJSON_ParseWithLength(body, len);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        back_off(ns, now_ms);           /* a proxy's error page, say: not the plugin */
        return;
    }
    ns->refused = false;
    ns->backoff_ms = 0;

    uint32_t ack;
    if (as_seq(cJSON_GetObjectItemCaseSensitive(obj, "ack"), &ack)) {
        acknowledged(ns, ack);
    }
    const cJSON *poll = cJSON_GetObjectItemCaseSensitive(obj, "poll_ms");
    ns->server_poll_ms = (cJSON_IsNumber(poll) && poll->valuedouble > 0 && poll->valuedouble < 3600000.0)
                         ? (uint32_t)poll->valuedouble : 0;

    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(obj, "cmds");
    const cJSON *cmd;
    if (!cJSON_IsArray(cmds)) {
        cmds = NULL;                    /* an object would be walked as one; it is not a list */
    }
    cJSON_ArrayForEach(cmd, cmds) {
        uint32_t seq;
        if (!cJSON_IsObject(cmd) || !as_seq(cJSON_GetObjectItemCaseSensitive(cmd, "seq"), &seq) || seq <= ns->cmd_ack) {
            continue;                   /* malformed, or acted on already */
        }
        if (!cJSON_PrintPreallocated((cJSON *)cmd, ns->cmd_buf, sizeof(ns->cmd_buf), false)) {
            ns->dropped++;              /* longer than any line the protocol takes: it would never fit */
            ns->cmd_ack = seq;
            continue;
        }
        if (on_cmd && !on_cmd(ctx, ns->cmd_buf, strlen(ns->cmd_buf))) {
            break;                      /* not taken: not acknowledged, so it comes again */
        }
        ns->cmd_ack = seq;
    }
    cJSON_Delete(obj);
    schedule_idle(ns);
}

const char *net_sync_link_state(const net_sync_t *ns)
{
    if (ns->refused) {
        return "refused";
    }
    if (ns->last_status == -1) {
        return "unreachable";
    }
    if (ns->last_status == 0) {
        return "idle";                  /* nothing tried yet */
    }
    return (ns->last_status >= 200 && ns->last_status < 300) ? "ok" : "error";
}
