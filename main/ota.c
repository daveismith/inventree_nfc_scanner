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

static const char *TAG = "ota";

static bool s_on_trial;
static bool s_reader_up;
static bool s_host_ok;
static volatile bool s_running;
static app_cmd_t s_cmd;                 /* the update being made: url and sha256 */

static void confirm_if_due(void)
{
    if (s_on_trial && s_reader_up && s_host_ok) {
        s_on_trial = false;
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "this firmware is confirmed; no rollback");
        }
    }
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_on_trial = true;
        ESP_LOGW(TAG, "running %s on trial: it rolls back unless the reader and a host come up", running->label);
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

/* The token goes along only to the plugin's own server. */
static esp_err_t add_auth(esp_http_client_handle_t client)
{
    const char *plugin = net_link_url();
    const char *host_end = strchr(plugin + (strncmp(plugin, "https://", 8) == 0 ? 8 : 7), '/');
    const size_t origin_len = host_end ? (size_t)(host_end - plugin) : strlen(plugin);
    if (plugin[0] && origin_len && strncmp(s_cmd.ota_url, plugin, origin_len) == 0 && net_link_token()[0]) {
        char auth[APP_NET_TOKEN_MAX + 8];
        snprintf(auth, sizeof(auth), "Token %s", net_link_token());
        esp_http_client_set_header(client, "Authorization", auth);
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
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        .http_client_init_cb = add_auth,
    };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    const char *failure = NULL;
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
        } else if (s_cmd.has_sha256 && !digest_matches(h)) {
            failure = "sha256 does not match";
        }
        if (failure) {
            esp_https_ota_abort(h);
        } else {
            err = esp_https_ota_finish(h);       /* checks the image and makes it the one to boot */
            if (err != ESP_OK) {
                failure = esp_err_to_name(err);
            }
        }
    }

    if (failure) {
        ESP_LOGE(TAG, "update failed: %s", failure);
        announce("failed", failure);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "update written (%d bytes); restarting into it", esp_https_ota_get_image_len_read(h));
    announce("restarting", NULL);
    vTaskDelay(pdMS_TO_TICKS(500));             /* let the lines out */
    esp_restart();
}

app_err_t ota_start(const app_cmd_t *cmd, const char **detail)
{
    if (s_running) {
        *detail = "an update is already in progress";
        return APP_ERR_BUSY;
    }
#if !CONFIG_APP_NET_ALLOW_HTTP
    if (strncmp(cmd->ota_url, "https://", 8) != 0) {
        *detail = "url: https only (CONFIG_APP_NET_ALLOW_HTTP permits http for testing)";
        return APP_ERR_BAD_ARG;
    }
#endif
    s_cmd = *cmd;
    s_running = true;
    if (xTaskCreate(ota_task, "ota", OTA_STACK, NULL, OTA_PRIO, NULL) != pdPASS) {
        s_running = false;
        *detail = "no memory for the update task";
        return APP_ERR_BUSY;
    }
    return APP_ERR_NONE;
}
