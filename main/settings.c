#include "settings.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define NAMESPACE   "app"
#define KEY_HID     "hid"

/* A bool Kconfig option that is off is not defined at all. */
#ifdef CONFIG_APP_HID_DEFAULT_ON
#define HID_DEFAULT_ON true
#else
#define HID_DEFAULT_ON false
#endif

static const char *TAG = "settings";

static bool s_ready;

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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
