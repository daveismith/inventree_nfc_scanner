#include "settings.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define NAMESPACE   "app"
#define KEY_HID     "hid"
#define KEY_NET     "net"               /* one blob: the whole settings_net_t */

/* A bool Kconfig option that is off is not defined at all. */
#ifdef CONFIG_APP_HID_DEFAULT_ON
#define HID_DEFAULT_ON true
#else
#define HID_DEFAULT_ON false
#endif

#ifndef CONFIG_APP_NET_POLL_MS
#define CONFIG_APP_NET_POLL_MS 1000
#endif
#ifndef CONFIG_APP_NET_WAIT_S
#define CONFIG_APP_NET_WAIT_S 25
#endif

static const char *TAG = "settings";

static bool s_ready;

void settings_init(void)
{
    /* With CONFIG_NVS_ENCRYPTION this also derives the keys, burning the eFuse HMAC key on
     * the first boot that finds its block empty. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND
            || err == ESP_ERR_NVS_CORRUPT_KEY_PART || err == ESP_ERR_NVS_INVALID_STATE) {
        /* Also what a partition written in the clear by an earlier firmware looks like. Nothing
         * in it is worth more than a working store. */
        ESP_LOGW(TAG, "NVS unreadable (%s): erasing", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    s_ready = err == ESP_OK;
    if (!s_ready) {
        ESP_LOGE(TAG, "no settings store (%s): using defaults", esp_err_to_name(err));
    }
}

bool settings_hid_default(void)
{
    bool enabled = HID_DEFAULT_ON;
    nvs_handle_t nvs;
    if (s_ready && nvs_open(NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value;
        if (nvs_get_u8(nvs, KEY_HID, &value) == ESP_OK) {
            enabled = value != 0;
        }
        nvs_close(nvs);
    }
    return enabled;
}

void settings_set_hid_default(bool enabled)
{
    nvs_handle_t nvs;
    if (!s_ready || nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(nvs, KEY_HID, enabled ? 1 : 0) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

void settings_net_load(settings_net_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = true;                /* on the air as soon as there is a network to join */
    out->poll_ms = CONFIG_APP_NET_POLL_MS;
    out->wait_s = CONFIG_APP_NET_WAIT_S;

    nvs_handle_t nvs;
    if (!s_ready || nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    settings_net_t stored;
    size_t len = sizeof(stored);
    if (nvs_get_blob(nvs, KEY_NET, &stored, &len) == ESP_OK && len == sizeof(stored)) {
        *out = stored;
        /* Whatever was stored, the strings end. */
        out->url[APP_NET_URL_MAX] = '\0';
        out->token[APP_NET_TOKEN_MAX] = '\0';
        for (int i = 0; i < SETTINGS_NETWORKS_MAX; i++) {
            out->networks[i].ssid[APP_NET_SSID_MAX] = '\0';
            out->networks[i].psk[APP_NET_PSK_MAX] = '\0';
        }
        if (out->preferred >= SETTINGS_NETWORKS_MAX) {
            out->preferred = 0;
        }
        if (out->poll_ms < 100) {
            out->poll_ms = CONFIG_APP_NET_POLL_MS;
        }
    }
    nvs_close(nvs);
}

void settings_net_save(const settings_net_t *net)
{
    nvs_handle_t nvs;
    if (!s_ready || nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "cannot save the network settings");
        return;
    }
    if (nvs_set_blob(nvs, KEY_NET, net, sizeof(*net)) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}
