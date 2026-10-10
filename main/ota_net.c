/* Updates fetched over the network (network builds only). See ota.h. */
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "net_link.h"
#include "ota.h"

#define OTA_STACK       8192
#define OTA_PRIO        3
#define OTA_TIMEOUT_MS  30000

static const char *TAG = "ota";

static app_cmd_t s_cmd;                 /* the update being made: url and sha256 */
static char s_plugin_url[APP_NET_URL_MAX + 1];
static char s_token[APP_NET_TOKEN_MAX + 1];

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

/* Why fetching failed: the image was not one for this chip, the flash would not take it, or
 * (anything else: the server, the connection, TLS) it could not be fetched. */
static app_err_t fetch_error(esp_err_t err)
{
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        return APP_ERR_VERIFY_FAILED;
    }
    if ((err & ~0xff) == ESP_ERR_FLASH_BASE || err == ESP_ERR_OTA_PARTITION_CONFLICT) {
        return APP_ERR_WRITE_FAILED;
    }
    return APP_ERR_DOWNLOAD_FAILED;
}

static void ota_task(void *arg)
{
    (void)arg;
    ota_announce("downloading", APP_ERR_NONE, NULL);

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
    app_err_t code = APP_ERR_NONE;      /* what `failure` is, for a program */
    int written = 0;
    if (err != ESP_OK) {
        failure = esp_err_to_name(err);
        code = fetch_error(err);
    } else {
        for (;;) {
            err = esp_https_ota_perform(h);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                break;
            }
        }
        if (err != ESP_OK) {
            failure = esp_err_to_name(err);
            code = fetch_error(err);
        } else if (!esp_https_ota_is_complete_data_received(h)) {
            failure = "the image was cut short";
            code = APP_ERR_DOWNLOAD_FAILED;
        } else if (!digest_matches(h)) {
            failure = "sha256 does not match";
            code = APP_ERR_VERIFY_FAILED;
        }
        written = esp_https_ota_get_image_len_read(h);
        if (failure) {
            esp_https_ota_abort(h);
        } else {
            err = esp_https_ota_finish(h);       /* checks the image and makes it the one to boot; frees h */
            if (err != ESP_OK) {
                failure = esp_err_to_name(err);
                code = err == ESP_ERR_OTA_VALIDATE_FAILED ? APP_ERR_VERIFY_FAILED : APP_ERR_WRITE_FAILED;
            }
        }
    }

    memset(s_token, 0, sizeof(s_token));
    if (failure) {
        ESP_LOGE(TAG, "update failed: %s", failure);
        ota_announce("failed", code, failure);
        ota_net_release();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "update written (%d bytes); restarting into it", written);
    ota_restart_when_free();
}

/*
 * What is trusted here. The plugin's server is trusted already: it hands out the tag
 * passwords and decides every job. So an update asked for over the network must come from
 * that same origin, over the link that is trusted to carry the token, and name the image's
 * digest. From USB, where someone is at the board, any allowed URL will do. There is no
 * signing: the digest proves the image is the one the server meant, not who built it. A
 * unit that must resist a hostile server needs secure boot, which is not in this firmware.
 */
app_err_t ota_net_start(const app_cmd_t *cmd, const char **detail)
{
    if (ota_in_progress()) {
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
    ota_net_claim();
    if (xTaskCreate(ota_task, "ota", OTA_STACK, NULL, OTA_PRIO, NULL) != pdPASS) {
        ota_net_release();
        memset(s_token, 0, sizeof(s_token));
        *detail = "no memory for the update task";
        return APP_ERR_BUSY;
    }
    return APP_ERR_NONE;
}
