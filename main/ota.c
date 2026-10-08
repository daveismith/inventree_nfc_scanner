#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#include "app_task.h"
#include "ota_stream.h"
#include "proto.h"
#if CONFIG_APP_NET_ENABLE
#include "net_link.h"
#endif

#define JOB_WAIT_MS     (10 * 60 * 1000)        /* a job still waiting for its tag ends by itself before this */
#define CONFIRM_DEADLINE_S (15 * 60)            /* on trial and still unconfirmed: give the old image its turn */

static const char *TAG = "ota";

/* A reader chip that never answers must not pin a unit on an unconfirmable image, so after
 * this long a reached host is enough on its own. */
#define READER_GRACE_S  60

static bool s_on_trial;
static bool s_reader_up;
static bool s_host_ok;
static bool s_usb_host_seen;
static bool s_net_settings_known;
static volatile bool s_net_running;     /* an image is being fetched over the network */
static ota_stream_t s_stream;           /* or sent over USB */

static void confirm_if_due(void)
{
    if (s_on_trial && s_reader_up && s_host_ok) {
        s_on_trial = false;
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "this firmware is confirmed; no rollback");
        }
    }
}

static void reader_grace_over(void *arg)
{
    (void)arg;
    if (!s_reader_up) {
        ESP_LOGW(TAG, "no reader chip after %d s; a reached host confirms this firmware on its own", READER_GRACE_S);
        ota_note_reader_up();
    }
}

/* An image nobody has reached in all this time is not working as a unit should: the next
 * boot returns to the previous image, which the bootloader does for an unconfirmed one. */
static void confirm_deadline(void *arg)
{
    (void)arg;
    if (s_on_trial) {
        ESP_LOGE(TAG, "unconfirmed after %d s: restarting so the previous firmware returns", CONFIRM_DEADLINE_S);
        esp_restart();
    }
}

static void ota_serial_init(void);

void ota_init(void)
{
    ota_serial_init();
#if !CONFIG_APP_NET_ENABLE
    s_net_settings_known = true;        /* there are none: a USB host is judged at once */
#endif
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_on_trial = true;
        ESP_LOGW(TAG, "running %s on trial: it rolls back unless the reader and a host come up", running->label);
        esp_timer_handle_t t;
        const esp_timer_create_args_t args = { .callback = reader_grace_over, .name = "ota_grace" };
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, (uint64_t)READER_GRACE_S * 1000000);
        }
        esp_timer_handle_t d;
        const esp_timer_create_args_t dargs = { .callback = confirm_deadline, .name = "ota_deadline" };
        if (esp_timer_create(&dargs, &d) == ESP_OK) {
            esp_timer_start_once(d, (uint64_t)CONFIRM_DEADLINE_S * 1000000);
        }
    }
}

void ota_note_reader_up(void)
{
    s_reader_up = true;
    confirm_if_due();
}

void ota_note_host_ok(void)
{
    s_host_ok = true;
    confirm_if_due();
}


static void judge_usb_host(void)
{
    /* A unit configured for the plugin proves itself by reaching the plugin; being plugged
     * into something that enumerates it says nothing about its network code. */
#if CONFIG_APP_NET_ENABLE
    const bool configured = net_link_configured();
#else
    const bool configured = false;
#endif
    if (s_usb_host_seen && s_net_settings_known && !configured) {
        ota_note_host_ok();
    }
}

void ota_note_usb_host(void)
{
    /* USB comes up before the network settings are read; the judgement waits for them. */
    s_usb_host_seen = true;
    judge_usb_host();
}

void ota_net_settings_known(void)
{
    s_net_settings_known = true;
    judge_usb_host();
}

bool ota_in_progress(void)
{
    return s_net_running || ota_stream_active(&s_stream);
}

bool ota_net_claim(void)
{
    if (ota_in_progress()) {
        return false;
    }
    s_net_running = true;
    return true;
}

void ota_net_release(void)
{
    s_net_running = false;
}

void ota_announce(const char *state, const char *detail)
{
    static char line[320];
    app_evt_t evt = { .type = APP_EVT_OTA, .origin = APP_ORIGIN_ALL, .state = state, .detail = detail };
    const size_t n = proto_format(&evt, line, sizeof(line));
    if (n > 0) {
        app_task_send(APP_ORIGIN_ALL, line, n);
    }
}


void ota_restart_when_free(void)
{
    /* Not in the middle of a tag write: a job that began before the update started may still
     * be running. New ones are refused while the update runs. */
    for (int waited = 0; app_task_job_active() && waited < JOB_WAIT_MS; waited += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ota_announce("restarting", NULL);
    vTaskDelay(pdMS_TO_TICKS(500));             /* let the lines out */
    esp_restart();
}

/*
 * The image over USB: written to the other slot as it arrives, hashed as it goes, checked
 * and made the one to boot at the end. The same slot, digest rule and trial period as an
 * update over the network.
 */
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static psa_hash_operation_t s_hash;

static bool serial_begin(void *ctx, uint32_t size, const char **detail)
{
    (void)ctx;
    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        *detail = "no slot to write an update to";
        return false;
    }
    if (size > s_part->size) {
        *detail = "size: larger than the application slot";
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        *detail = "no hash";
        return false;
    }
    s_hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&s_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        *detail = "no hash";
        return false;
    }
    /* Sequential writes erase as they go, so no command waits for the whole slot's erase. */
    if (esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle) != ESP_OK) {
        psa_hash_abort(&s_hash);
        *detail = "could not start writing the slot";
        return false;
    }
    ESP_LOGI(TAG, "update over USB: %lu bytes into %s", (unsigned long)size, s_part->label);
    ota_announce("downloading", NULL);
    return true;
}

static bool serial_write(void *ctx, const uint8_t *data, size_t len, const char **detail)
{
    (void)ctx;
    if (esp_ota_write(s_handle, data, len) != ESP_OK) {
        *detail = "flash write failed";
        return false;
    }
    if (psa_hash_update(&s_hash, data, len) != PSA_SUCCESS) {
        *detail = "hash failed";
        return false;
    }
    return true;
}

static bool serial_finish(void *ctx, const uint8_t sha256[32], const char **detail)
{
    (void)ctx;
    uint8_t got[32];
    size_t got_len = 0;
    if (psa_hash_finish(&s_hash, got, sizeof(got), &got_len) != PSA_SUCCESS || got_len != sizeof(got)
        || memcmp(got, sha256, sizeof(got)) != 0) {
        esp_ota_abort(s_handle);
        *detail = "sha256 does not match";
        ota_announce("failed", *detail);
        return false;
    }
    esp_err_t err = esp_ota_end(s_handle);      /* checks the image itself; frees the handle */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_part);
    }
    if (err != ESP_OK) {
        *detail = err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a valid image for this chip" : esp_err_to_name(err);
        ota_announce("failed", *detail);
        return false;
    }
    return true;
}

static void serial_abort(void *ctx)
{
    (void)ctx;
    esp_ota_abort(s_handle);
    psa_hash_abort(&s_hash);
}

static void restart_task(void *arg)
{
    (void)arg;
    ota_restart_when_free();
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ota_serial_init(void)
{
    const ota_stream_backend_t backend = {
        .begin = serial_begin, .write = serial_write, .finish = serial_finish, .abort = serial_abort,
    };
    ota_stream_init(&s_stream, &backend);
}

app_err_t ota_command(const app_cmd_t *cmd, const char **detail)
{
    if (cmd->type == APP_CMD_OTA) {
#if CONFIG_APP_NET_ENABLE
        return ota_net_start(cmd, detail);
#else
        *detail = "this build has no network";
        return APP_ERR_UNKNOWN_CMD;
#endif
    }
    if (cmd->type == APP_CMD_OTA_BEGIN && s_net_running) {
        *detail = "an update over the network is in progress";
        return APP_ERR_BUSY;
    }
    const bool was_active = ota_stream_active(&s_stream);
    const app_err_t err = ota_stream_command(&s_stream, cmd, now_ms(), detail);
    if (err == APP_ERR_NONE && cmd->type == APP_CMD_OTA_END) {
        ESP_LOGI(TAG, "update over USB written; restarting into it");
        if (xTaskCreate(restart_task, "ota_restart", 3072, NULL, 3, NULL) != pdPASS) {
            ota_restart_when_free();
        }
    } else if (err != APP_ERR_NONE && was_active && !ota_stream_active(&s_stream)
               && cmd->type != APP_CMD_OTA_END) {
        ota_announce("failed", *detail);        /* ota_end's failures announce themselves */
    }
    return err;
}

void ota_link_down(uint8_t origin)
{
    if (ota_stream_link_down(&s_stream, origin)) {
        ota_announce("failed", "the link went down mid-update");
    }
}

void ota_poll(void)
{
    if (ota_stream_expire(&s_stream, now_ms())) {
        ota_announce("failed", "nothing more was sent");
    }
}
