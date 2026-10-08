/*
 * The device's behaviour, with nothing in it that needs the board.
 *
 * One task owns an app_core_t and feeds it three things: commands, what the tag poller sees,
 * and the passage of time. It answers through the callbacks in app_env_t. Tag operations run
 * inside the calls below, over the transceiver they are handed, so a program job is done by
 * the time app_core_tag_arrived() returns.
 *
 *   idle (lookup) --program/wipe--> waiting --tag--> writes --> idle
 *                                     | cancel / timeout ----> idle
 *
 * In idle, a tag is read and reported, and its text record typed if HID is on. While a job
 * waits, the first tag presented decides the job's outcome and nothing is typed.
 */
#pragma once

#include "app_types.h"
#include "ndef.h"
#include "ntag21x.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the user should see and hear. IDLE, JOB_WAITING, NFC_ERROR and BOOTLOADER are states
 * that persist; the rest are moments. */
typedef enum {
    APP_FB_IDLE,
    APP_FB_JOB_WAITING,
    APP_FB_NFC_ERROR,
    APP_FB_BOOTLOADER,
    APP_FB_TAG_OK,
    APP_FB_TAG_UNKNOWN,
    APP_FB_JOB_WRITING,
    APP_FB_JOB_DONE,
    APP_FB_JOB_FAILED,
} app_feedback_t;

typedef struct {
    void *ctx;
    void (*emit)(void *ctx, const app_evt_t *evt);
    uint32_t (*now_ms)(void *ctx);
    void (*sysinfo)(void *ctx, app_sysinfo_t *out);
    void (*feedback)(void *ctx, app_feedback_t fb);             /* optional */
    void (*hid_type)(void *ctx, const char *text);              /* optional */
    void (*hid_save_default)(void *ctx, bool enabled);          /* optional */
    void (*set_log_level)(void *ctx, app_log_level_t level);    /* optional */
    void (*enter_bootloader)(void *ctx);                        /* optional */
    void (*debug)(void *ctx, app_debug_action_t action);        /* optional; absent, `debug` is unknown */
    /* Optional; absent, `net` and `ota` are unknown commands. On success `net` fills in the
     * status for the answer; on failure both may name a detail. */
    app_err_t (*net)(void *ctx, const app_cmd_t *cmd, app_net_status_t *status, const char **detail);
    app_err_t (*ota)(void *ctx, const app_cmd_t *cmd, const char **detail);
    /* Optional: an update is being fetched, which ends in a restart; no job may begin. */
    bool (*updating)(void *ctx);
} app_env_t;

/* A tag as the reader's anticollision saw it. */
typedef struct {
    uint8_t uid_len;
    uint8_t uid[APP_UID_MAX];
    uint8_t sak;
    uint16_t atqa;
} app_tag_t;

typedef struct {
    app_env_t env;
    bool nfc_ok;
    bool hid_default;
    bool hid_override_set;      /* a `hid` command this session, until the link that sent it drops */
    bool hid_override;
    uint8_t hid_override_origin;
    uint8_t auth0;
    bool tag_here;              /* a tag is on the reader, and has been reported or written */
    app_tag_t here;
    bool job_active;
    uint32_t job_deadline;
    app_cmd_t job;
    ntag_t tag;
    ndef_info_t ndef;
} app_core_t;

void app_core_init(app_core_t *core, const app_env_t *env, bool hid_default);

void app_core_command(app_core_t *core, const app_cmd_t *cmd);

/* Call regularly: this is where a waiting job times out. */
void app_core_tick(app_core_t *core);

/* A job is waiting. The poller uses this to offer a tag that was already on the reader when
 * the job began, by calling app_core_tag_arrived() for it again. */
bool app_core_wants_tag(const app_core_t *core);

/*
 * Once per arrival, or again when app_core_wants_tag(). Reads or writes the tag over `x`.
 * Returns false when the tag slipped away before it could be read and nothing was reported:
 * the caller should treat it as not having arrived yet, and look again.
 */
bool app_core_tag_arrived(app_core_t *core, const app_tag_t *tag, nfc_xcvr_t *x);

/* More than one tag is in the field. Once per occurrence. */
void app_core_tag_conflict(app_core_t *core);

void app_core_tag_removed(app_core_t *core, const app_tag_t *tag);

/* The reader started or stopped answering. */
void app_core_nfc_state(app_core_t *core, bool ok);

/*
 * A host opened or closed a link. Up, it is greeted with `hello`. Down, what it owned goes
 * with it: a job it started and is no longer there to see through is cancelled, and a
 * per-session `hid` setting ends. A link that is never closed, such as the plugin's, keeps
 * its jobs through a lapse in the network and leaves them to their own timeout.
 */
void app_core_link(app_core_t *core, uint8_t origin, bool up);

bool app_core_hid_enabled(const app_core_t *core);
const char *app_core_state_name(const app_core_t *core);

#ifdef __cplusplus
}
#endif
