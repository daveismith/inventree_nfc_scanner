#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
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
#include "net_link.h"
#include "proto.h"

#define OTA_STACK       8192
#define OTA_PRIO        3
#define OTA_TIMEOUT_MS  30000
#define JOB_WAIT_MS     (10 * 60 * 1000)        /* a job still waiting for its tag ends by itself before this */
#define CONFIRM_DEADLINE_S (15 * 60)            /* on trial and still unconfirmed: give the old image its turn */

static const char *TAG = "ota";

/* A reader chip that never answers must not pin a unit on an unconfirmable image, so after
 * this long a reached host is enough on its own. */
#define READER_GRACE_S  60

static bool s_on_trial;
static bool s_reader_up;
static bool s_host_ok;
static volatile bool s_running;
static app_cmd_t s_cmd;                 /* the update being made: url and sha256 */
static char s_plugin_url[APP_NET_URL_MAX + 1];
static char s_token[APP_NET_TOKEN_MAX + 1];

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

void ota_init(void)
{
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

bool ota_in_progress(void)
{
    return s_running;
}

static void announce(const char *state, const char *detail)
{
    static char line[320];
    app_evt_t evt = { .type = APP_EVT_OTA, .origin = APP_ORIGIN_ALL, .state = state, .detail = detail };
    const size_t n = proto_format(&evt, line, sizeof(line));
    if (n > 0) {
        app_task_send(APP_ORIGIN_ALL, line, n);
    }
}

/*
 * The token goes along only to the plugin's own origin: same scheme, host and port. That is
 * decided again on every connection, since the updater follows redirects, and a redirect to
 * another host must not carry it.
 */
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_CONNECTED) {
        return ESP_OK;
    }
    char url[APP_OTA_URL_MAX + 1] = { 0 };
    esp_http_client_get_url(evt->client, url, sizeof(url));
    if (s_plugin_url[0] && s_token[0] && same_origin(url, s_plugin_url)) {
        char auth[APP_NET_TOKEN_MAX + 8];
        snprintf(auth, sizeof(auth), "Token %s", s_token);
        esp_http_client_set_header(evt->client, "Authorization", auth);
        memset(auth, 0, sizeof(auth));
    } else {
        esp_http_client_delete_header(evt->client, "Authorization");
    }
    return ESP_OK;
}

/* SHA-256 of the image just written, compared with what the command said. */
static bool digest_matches(esp_https_ota_handle_t h)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    const int len = esp_https_ota_get_image_len_read(h);
    if (part == NULL || len <= 0 || psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    static uint8_t buf[4096];
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    for (int at = 0; at < len; at += (int)sizeof(buf)) {
        const size_t n = (size_t)(len - at) < sizeof(buf) ? (size_t)(len - at) : sizeof(buf);
        if (esp_partition_read(part, (size_t)at, buf, n) != ESP_OK || psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return false;
        }
    }
    uint8_t out[32];
    size_t out_len = 0;
    if (psa_hash_finish(&op, out, sizeof(out), &out_len) != PSA_SUCCESS || out_len != sizeof(out)) {
        return false;
    }
    return memcmp(out, s_cmd.sha256, sizeof(out)) == 0;
}

static void ota_task(void *arg)
{
    (void)arg;
    announce("downloading", NULL);

    esp_http_client_config_t http = {
        .url = s_cmd.ota_url,
        .timeout_ms = OTA_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .event_handler = on_http_event,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    const char *failure = NULL;
    int written = 0;
    if (err != ESP_OK) {
        failure = esp_err_to_name(err);
    } else {
        for (;;) {
            err = esp_https_ota_perform(h);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                break;
            }
        }
        if (err != ESP_OK) {
            failure = esp_err_to_name(err);
        } else if (!esp_https_ota_is_complete_data_received(h)) {
            failure = "the image was cut short";
        } else if (!digest_matches(h)) {
            failure = "sha256 does not match";
        }
        written = esp_https_ota_get_image_len_read(h);
        if (failure) {
            esp_https_ota_abort(h);
        } else {
            err = esp_https_ota_finish(h);       /* checks the image and makes it the one to boot; frees h */
            if (err != ESP_OK) {
                failure = esp_err_to_name(err);
            }
        }
    }

    memset(s_token, 0, sizeof(s_token));
    if (failure) {
        ESP_LOGE(TAG, "update failed: %s", failure);
        announce("failed", failure);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "update written (%d bytes); restarting into it", written);
    /* Not in the middle of a tag write: a job that began before the download started may
     * still be running. New ones are refused while the download runs. */
    for (int waited = 0; app_task_job_active() && waited < JOB_WAIT_MS; waited += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    announce("restarting", NULL);
    vTaskDelay(pdMS_TO_TICKS(500));             /* let the lines out */
    esp_restart();
}

/*
 * What is trusted here. The plugin's server is trusted already: it hands out the tag
 * passwords and decides every job. So an update asked for over the network must come from
 * that same origin, over the link that is trusted to carry the token, and name the image's
 * digest. From USB, where someone is at the board, any allowed URL will do. There is no
 * signing: the digest proves the image is the one the server meant, not who built it. A
 * unit that must resist a hostile server needs secure boot, which is not in this firmware.
 */
app_err_t ota_start(const app_cmd_t *cmd, const char **detail)
{
    if (s_running) {
        *detail = "an update is already in progress";
        return APP_ERR_BUSY;
    }
    if (!url_allowed(cmd->ota_url)) {
        *detail = "url: https only, with no user info (CONFIG_APP_NET_ALLOW_HTTP permits http for testing)";
        return APP_ERR_BAD_ARG;
    }
    net_link_server(s_plugin_url, sizeof(s_plugin_url), s_token, sizeof(s_token));
    if (cmd->remote && !same_origin(cmd->ota_url, s_plugin_url)) {
        memset(s_token, 0, sizeof(s_token));
        *detail = "from the network, an image must come from the plugin's own server";
        return APP_ERR_NOT_ALLOWED;
    }
    s_cmd = *cmd;
    s_running = true;
    if (xTaskCreate(ota_task, "ota", OTA_STACK, NULL, OTA_PRIO, NULL) != pdPASS) {
        s_running = false;
        memset(s_token, 0, sizeof(s_token));
        *detail = "no memory for the update task";
        return APP_ERR_BUSY;
    }
    return APP_ERR_NONE;
}
