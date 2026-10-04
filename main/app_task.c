#include "app_task.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_core.h"
#include "app_download_mode.h"
#include "dev_recovery.h"
#include "feedback.h"
#include "log_forward.h"
#include "pn532.h"
#include "proto.h"
#include "settings.h"
#include "sysinfo.h"
#include "usb_dev.h"

_Static_assert(USB_CDC_LINE_MAX == PROTO_LINE_MAX, "the link and the protocol must agree on a line");
_Static_assert(PN532_UID_MAX == APP_UID_MAX, "a UID must fit an event");

#define TASK_STACK      6144
#define TASK_PRIO       5
#define CMD_QUEUE_LEN   3
#define SEND_WAIT_MS    100

/*
 * How often the reader is asked. A poll that finds nothing takes about 19 ms (measured), so
 * ten a second leaves the bus idle most of the time and a tap is noticed within a tenth of a
 * second; twice that while a job is waiting for its tag.
 */
#define POLL_IDLE_MS        100
#define POLL_WAITING_MS     50
#define PRESENCE_MS         150     /* how often a tag that is here is checked to still be here */
#define PRESENCE_MISSES     3       /* checks in a row it must fail before it counts as gone */
#define NFC_RETRY_MS        2000    /* how often a reader that is not answering is tried again */
#define NFC_FAILS_BEFORE_DOWN 5
#define SLOW_POLL_MS        100     /* a poll slower than this is worth a debug line */

/* `hello` waits this long after the port opens. Sent at once it is lost: pyserial, for one,
 * discards whatever arrived in its first few milliseconds as part of opening the port. */
#define HELLO_DELAY_MS      200

/* Task notification bits */
#define NOTE_CMD        (1u << 0)
#define NOTE_LINK_DOWN  (1u << 1)
#define NOTE_LINK_UP    (1u << 2)

static const char *TAG = "app";

static app_core_t s_core;
static QueueHandle_t s_cmds;
static TaskHandle_t s_task;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void send_event(const app_evt_t *evt, char *buf, size_t cap)
{
    const size_t n = proto_format(evt, buf, cap);
    if (n == 0) {
        ESP_LOGW(TAG, "event %d did not fit a line", (int)evt->type);
        return;
    }
    usb_cdc_send(buf, n, SEND_WAIT_MS);
}

/* app_core's environment. All of it runs on the app task. */

static void env_emit(void *ctx, const app_evt_t *evt)
{
    (void)ctx;
    static char line[1024];
    send_event(evt, line, sizeof(line));
}

static uint32_t env_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)now_ms();
}

static void env_sysinfo(void *ctx, app_sysinfo_t *out)
{
    (void)ctx;
    sysinfo_get(out);
}

static void env_feedback(void *ctx, app_feedback_t fb)
{
    (void)ctx;
    feedback_show(fb);
}

static void env_hid_type(void *ctx, const char *text)
{
    (void)ctx;
    if (!usb_hid_type(text)) {
        ESP_LOGW(TAG, "could not type \"%s\": the host is not taking keystrokes", text);
    }
}

static void env_hid_save_default(void *ctx, bool enabled)
{
    (void)ctx;
    settings_set_hid_default(enabled);
}

static void env_set_log_level(void *ctx, app_log_level_t level)
{
    (void)ctx;
    log_forward_set_level(level);
}

static void env_enter_bootloader(void *ctx)
{
    (void)ctx;
    app_download_mode_request();
}

static void env_debug(void *ctx, app_debug_action_t action)
{
    (void)ctx;
    dev_recovery_debug(action);
}

void app_task_init(void)
{
    s_cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(app_cmd_t));
    configASSERT(s_cmds);

    const app_env_t env = {
        .emit = env_emit,
        .now_ms = env_now_ms,
        .sysinfo = env_sysinfo,
        .feedback = env_feedback,
        .hid_type = env_hid_type,
        .hid_save_default = env_hid_save_default,
        .set_log_level = env_set_log_level,
        .enter_bootloader = env_enter_bootloader,
        .debug = dev_recovery_debug_available() ? env_debug : NULL,
    };
    app_core_init(&s_core, &env, false);
}

/*
 * The reader. One step is at most one poll or one presence check, so the loop below keeps
 * coming back for commands; a tag operation, which is the exception, runs inside
 * app_core_tag_arrived().
 */
#if CONFIG_APP_NFC_ENABLE

static bool s_nfc_up;
static int s_nfc_fails;
static int64_t s_nfc_retry_at;
static bool s_tag_present;
static bool s_conflict;
static int64_t s_presence_at;
static int s_presence_misses;
static app_tag_t s_tag;

static void nfc_down(void)
{
    s_nfc_up = false;
    s_tag_present = false;
    s_conflict = false;
    s_nfc_retry_at = now_ms();          /* try to start it again at once; then every NFC_RETRY_MS */
    sysinfo_set_pn532(false, 0, 0, 0);
    ESP_LOGE(TAG, "the PN532 has stopped answering");
    app_core_nfc_state(&s_core, false);
}

static void nfc_step(void)
{
    const int64_t now = now_ms();

    if (!s_nfc_up) {
        if (now < s_nfc_retry_at) {
            return;
        }
        pn532_version_t v;
        if (pn532_start(&v) != ESP_OK) {
            s_nfc_retry_at = now + NFC_RETRY_MS;
            return;
        }
        ESP_LOGI(TAG, "PN532 firmware %u.%u (IC 0x%02x)", v.ver, v.rev, v.ic);
        s_nfc_up = true;
        s_nfc_fails = 0;
        sysinfo_set_pn532(true, v.ic, v.ver, v.rev);
        app_core_nfc_state(&s_core, true);
        return;
    }

    if (!s_tag_present) {
        pn532_target_t t;
        const esp_err_t err = pn532_poll(&t);
        const int took_ms = (int)(now_ms() - now);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "poll failed after %d ms (%s), %d in a row", took_ms, esp_err_to_name(err), s_nfc_fails + 1);
            pn532_recover();
            if (++s_nfc_fails >= NFC_FAILS_BEFORE_DOWN) {
                nfc_down();
            }
            return;
        }
        s_nfc_fails = 0;
        if (t.count > 0 || took_ms > SLOW_POLL_MS) {
            ESP_LOGD(TAG, "poll: %u tag(s) in %d ms", t.count, took_ms);
        }
        if (t.count == 0) {
            s_conflict = false;
            return;
        }
        if (t.count > 1) {
            if (!s_conflict) {
                s_conflict = true;      /* said once, until the field is clear again */
                app_core_tag_conflict(&s_core);
            }
            pn532_release();
            return;
        }
        s_conflict = false;
        s_tag = (app_tag_t){ .uid_len = t.uid_len, .sak = t.sak, .atqa = t.atqa };
        memcpy(s_tag.uid, t.uid, t.uid_len);
        const bool handled = app_core_tag_arrived(&s_core, &s_tag, pn532_xcvr());
        ESP_LOGD(TAG, "tag %s in %d ms", handled ? "handled" : "not read", (int)(now_ms() - now) - took_ms);
        if (!handled) {
            /* Still on its way in. Nothing was said about it, so there is nothing to take
             * back: the next poll finds it again. */
            pn532_release();
            return;
        }
        s_tag_present = true;
        s_presence_misses = 0;
        s_presence_at = now_ms() + PRESENCE_MS;
        return;
    }

    /* A job that began with a tag already on the reader gets that tag. */
    if (app_core_wants_tag(&s_core)) {
        app_core_tag_arrived(&s_core, &s_tag, pn532_xcvr());
        s_presence_misses = 0;
        s_presence_at = now_ms() + PRESENCE_MS;
        return;
    }
    if (now >= s_presence_at) {
        s_presence_at = now + PRESENCE_MS;
        /* A tag being lifted, or one pressed against the antenna, drops the odd exchange.
         * Reporting each as a removal would turn one tap into several. */
        if (pn532_present(s_tag.uid_len == 7 && s_tag.sak == 0x00)) {
            s_presence_misses = 0;
        } else if (++s_presence_misses >= PRESENCE_MISSES) {
            s_tag_present = false;
            app_core_tag_removed(&s_core, &s_tag);
            pn532_release();
        }
    }
}

static uint32_t nfc_interval_ms(void)
{
    return (s_tag_present || app_core_wants_tag(&s_core)) ? POLL_WAITING_MS : POLL_IDLE_MS;
}

#else /* !CONFIG_APP_NFC_ENABLE */

static void nfc_step(void) { }
static uint32_t nfc_interval_ms(void) { return POLL_IDLE_MS; }

#endif

static void app_task(void *arg)
{
    (void)arg;
    static app_cmd_t cmd;
    int64_t hello_at = 0;               /* 0: none owed */

    for (;;) {
        uint32_t notes = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notes, pdMS_TO_TICKS(nfc_interval_ms()));

        /* A close and a reopen can both land in one wake-up; in that order they are a new
         * session, which must not inherit the last one's settings. */
        if (notes & NOTE_LINK_DOWN) {
            app_core_link(&s_core, false);
            hello_at = 0;
        }
        if (notes & NOTE_LINK_UP) {
            hello_at = now_ms() + HELLO_DELAY_MS;
        }
        if (hello_at && now_ms() >= hello_at) {
            hello_at = 0;
            if (usb_cdc_connected()) {
                app_core_link(&s_core, true);
            }
        }

        while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
            app_core_command(&s_core, &cmd);
        }
        app_core_tick(&s_core);
        nfc_step();
    }
}

void app_task_start(void)
{
    /* Read here rather than in app_task_init(): the settings store is opened after USB is
     * up, so that nothing it does can keep the board off the bus. */
    if (usb_hid_available()) {
        s_core.hid_default = settings_hid_default();
    } else {
        s_core.env.hid_type = NULL;         /* this build has no keyboard to type with */
    }
    feedback_show(APP_FB_NFC_ERROR);        /* until the reader answers */

    const BaseType_t made = xTaskCreate(app_task, "app", TASK_STACK, NULL, TASK_PRIO, &s_task);
    configASSERT(made == pdPASS);
}

/* USB receive task from here down to app_task_line_too_long(). */

static void reply_error(app_err_t error, const char *cmd, bool has_id, int32_t id, const char *detail)
{
    static char line[256];
    proto_err_t err = { .error = error, .cmd = cmd, .has_id = has_id, .id = id };
    strlcpy(err.detail, detail ? detail : "", sizeof(err.detail));
    app_evt_t evt;
    proto_err_event(&err, &evt);
    send_event(&evt, line, sizeof(line));
}

void app_task_line(const char *line, size_t len)
{
    static app_cmd_t cmd;
    proto_err_t err;
    if (!proto_parse(line, len, &cmd, &err)) {
        reply_error(err.error, err.cmd, err.has_id, err.id, err.detail);
        return;
    }
    /* The app task comes back for commands between reader steps; a queue still full after
     * two seconds means it is stuck, and saying so beats leaving the host without an answer. */
    if (s_task == NULL || xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
        reply_error(APP_ERR_BUSY, app_cmd_name(cmd.type), cmd.has_id, cmd.id, NULL);
        return;
    }
    xTaskNotify(s_task, NOTE_CMD, eSetBits);
}

void app_task_line_too_long(void)
{
    reply_error(APP_ERR_LINE_TOO_LONG, NULL, false, 0, NULL);
}

/* TinyUSB task: signal only. */
void app_task_link(bool up)
{
    if (s_task) {
        xTaskNotify(s_task, up ? NOTE_LINK_UP : NOTE_LINK_DOWN, eSetBits);
    }
}
