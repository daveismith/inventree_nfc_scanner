/*
 * The exchange with the InvenTree plugin, with no network in it.
 *
 * The reader is an HTTP client of the plugin's `/sync` endpoint (docs/network-transport-plan.md
 * and the plugin's docs/api.md). This keeps the part with the logic in it: the queue of
 * messages not yet acknowledged, their numbering, which commands are new, how soon to call
 * again, and when to stop calling. Building the body and reading the answer are done here;
 * the HTTP request itself is the caller's, which is what lets the host tests drive this with
 * strings and the simulator with a plain socket.
 *
 * One net_sync_t may be used from more than one task; the caller serialises access.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_SYNC_PROTO          1
#define NET_SYNC_READER_MAX     32
#define NET_SYNC_FW_MAX         32
#define NET_SYNC_MSG_MAX        2080    /* one queued message: the protocol's longest line, with its "seq" */
#define NET_SYNC_QUEUE_LEN      12
#define NET_SYNC_CMD_MAX        2048    /* a command as handed on: the protocol's longest line */

#define NET_SYNC_BACKOFF_MIN_MS 1000
#define NET_SYNC_BACKOFF_MAX_MS 30000
#define NET_SYNC_REFUSED_RETRY_MS 60000 /* after a 401, 403 or 404: the server has to be reconfigured, or we do */

typedef struct {
    uint32_t seq;
    bool keep;                          /* a job's answer or result: never dropped for room */
    uint16_t len;
    char json[NET_SYNC_MSG_MAX];        /* the object with "seq" put first; no terminator */
} net_sync_msg_t;

typedef struct {
    char reader[NET_SYNC_READER_MAX];
    char fw[NET_SYNC_FW_MAX];            /* the firmware version, so the server knows what each scanner runs */
    uint32_t boot;
    uint32_t last_seq;                  /* of the messages queued so far */
    uint32_t cmd_ack;                   /* the highest command seq acted on: the `ack` sent */

    net_sync_msg_t queue[NET_SYNC_QUEUE_LEN];   /* a ring, oldest at head */
    uint8_t head;
    uint8_t count;
    uint32_t dropped;

    uint32_t poll_ms;                   /* the idle interval; never called more often than this */
    uint32_t wait_s;                    /* the hold asked for; 0 is plain polling */
    uint32_t server_poll_ms;            /* an interval the server asked for, or 0 */
    uint32_t backoff_ms;                /* 0 when the last call went through */
    bool refused;                       /* told to go away; waits NET_SYNC_REFUSED_RETRY_MS */
    int last_status;                    /* HTTP status of the last answer; -1 unreachable; 0 none yet */
    uint32_t last_started;              /* when the last call began, ms */
    uint32_t next_at;                   /* when the next may begin, ms */
    uint32_t sent_through;              /* the newest message seq carried by a call so far */
    uint32_t generation;                /* bumped by net_sync_new_server: an answer from before is not for us */
    bool ever_called;

    char cmd_buf[NET_SYNC_CMD_MAX];     /* a command being handed on */
} net_sync_t;

/* Returns false when it cannot take the command now; it is then not acknowledged, and the
 * server sends it again. */
typedef bool (*net_sync_cmd_fn)(void *ctx, const char *json, size_t len);

/* `boot` is a number new to this run of the firmware, so the server knows the numbering
 * started again. `poll_ms` and `wait_s` are the configured pacing. */
void net_sync_init(net_sync_t *ns, const char *reader, const char *fw, uint32_t boot, uint32_t poll_ms, uint32_t wait_s);

void net_sync_set_pacing(net_sync_t *ns, uint32_t poll_ms, uint32_t wait_s);

/* Settings changed: a refusal no longer stands, and the next call may go at once. */
void net_sync_reconfigured(net_sync_t *ns);

/* A different server: what was queued for the old one is dropped, and command numbering
 * starts again, so the new server's first command is not mistaken for one already done. */
void net_sync_new_server(net_sync_t *ns);

/*
 * A line the state machine emitted for this link (with or without its '\n'). What the
 * server wants is queued, numbered; `hello`, `tag_removed`, `log` and `net` are not for it.
 * Returns true when queued. For room, taps go first; a job's answer or result is dropped
 * only when the queue holds nothing but those, which takes NET_SYNC_QUEUE_LEN of them
 * unacknowledged. `dropped` counts every message lost for room or for size.
 */
bool net_sync_queue(net_sync_t *ns, const char *line, size_t len);

/* Something is waiting to be reported. */
bool net_sync_has_pending(const net_sync_t *ns);

/* Whether a call may begin now, and if not, how long until it may. */
bool net_sync_due(const net_sync_t *ns, uint32_t now_ms);
uint32_t net_sync_delay_ms(const net_sync_t *ns, uint32_t now_ms);

/*
 * Build the body of a call beginning now. `hold` asks for the configured hold (long polling);
 * false asks the server to answer at once, which is right for a call made to report. The
 * queue goes oldest first, as much of it as fits `cap`; the rest waits for the next call.
 * Returns the body's length, or 0 only when not even an empty call fits. Marks the call as
 * started.
 */
size_t net_sync_request(net_sync_t *ns, uint32_t now_ms, bool hold, char *out, size_t cap);

/* The value to remember when a call begins; an answer that arrives after it has changed
 * belongs to a server that is no longer ours, and must not be given to net_sync_response. */
uint32_t net_sync_generation(const net_sync_t *ns);

/*
 * The server's answer: HTTP `status` and the body. On 200 the acknowledged messages are
 * dropped and each command not yet acted on is handed to `on_cmd`, oldest first (its `seq`
 * is left in; the protocol ignores it). A command too long for the protocol's line is
 * acknowledged and skipped, with `dropped` counting it. Other statuses set the pacing: 401,
 * 403 and 404 are a refusal, the rest a back-off. `seq` and `ack` must be whole numbers;
 * `cmds` must be an array.
 */
void net_sync_response(net_sync_t *ns, uint32_t now_ms, int status, const char *body, size_t len,
                       net_sync_cmd_fn on_cmd, void *ctx);

/* The call did not reach the server at all. */
void net_sync_unreachable(net_sync_t *ns, uint32_t now_ms);

/* For `net`: "off" and "no_wifi" are the caller's to say; this gives "idle" (nothing tried
 * yet), "ok", "unreachable", "refused" or "error". */
const char *net_sync_link_state(const net_sync_t *ns);

#ifdef __cplusplus
}
#endif
