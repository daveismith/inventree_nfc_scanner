#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi";

static wifi_sta_on_change_t s_on_change;
static void *s_ctx;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry;

static wifi_sta_network_t s_networks[WIFI_POLICY_NETWORKS_MAX];
static wifi_policy_t s_policy;
static bool s_enabled;
static wifi_sta_state_t s_state = WIFI_STA_OFF;
static char s_ip[16];
static int s_last_joined = -1;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_state(wifi_sta_state_t state)
{
    if (s_state == state) {
        return;
    }
    s_state = state;
    ESP_LOGI(TAG, "%s", wifi_sta_state_name(state));
    if (s_on_change) {
        s_on_change(s_ctx, state);
    }
}

/* Under s_lock. Begin a join of the policy's current network. */
static void connect_now(void)
{
    const int i = wifi_policy_next(&s_policy);
    if (i < 0) {
        set_state(WIFI_STA_NO_NETWORK);
        return;
    }
    wifi_config_t cfg = { 0 };
    /* The driver's fields are not strings: 32 and 64 bytes, filled, with no terminator. */
    memcpy(cfg.sta.ssid, s_networks[i].ssid, strnlen(s_networks[i].ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, s_networks[i].psk, strnlen(s_networks[i].psk, sizeof(cfg.sta.password)));
    cfg.sta.threshold.authmode = s_networks[i].psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    set_state(WIFI_STA_CONNECTING);
    ESP_LOGI(TAG, "joining \"%s\"", s_networks[i].ssid);
    esp_wifi_connect();
}

/* Under s_lock. A join or a rejoin is wanted: now, or when the policy says. */
static void schedule(void)
{
    esp_timer_stop(s_retry);
    if (!s_enabled || s_policy.up) {
        return;
    }
    const uint32_t delay = wifi_policy_delay_ms(&s_policy, now_ms());
    if (delay == UINT32_MAX) {
        set_state(WIFI_STA_NO_NETWORK);
        return;
    }
    if (delay == 0) {
        connect_now();
    } else {
        ESP_LOGI(TAG, "next try in %lu ms", (unsigned long)delay);
        esp_timer_start_once(s_retry, (uint64_t)delay * 1000);
    }
}

static void on_retry(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_enabled && !s_policy.up) {
        connect_now();
    }
    xSemaphoreGive(s_lock);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_enabled) {
            if (s_state == WIFI_STA_CONNECTED) {
                ESP_LOGW(TAG, "link lost (reason %u)", d->reason);
                wifi_policy_link_lost(&s_policy, now_ms());
            } else {
                ESP_LOGW(TAG, "join failed (reason %u)", d->reason);
                wifi_policy_join_failed(&s_policy, now_ms());
            }
            s_ip[0] = '\0';
            set_state(WIFI_STA_CONNECTING);
            schedule();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_last_joined = wifi_policy_next(&s_policy);
        wifi_policy_joined(&s_policy);
        ESP_LOGI(TAG, "address %s", s_ip);
        set_state(WIFI_STA_CONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_ip[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

esp_err_t wifi_sta_init(wifi_sta_on_change_t on_change, void *ctx)
{
    s_on_change = on_change;
    s_ctx = ctx;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     /* the firmware keeps its own settings */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    const esp_timer_create_args_t t = { .callback = on_retry, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_retry));
    bool none[WIFI_POLICY_NETWORKS_MAX] = { 0 };
    wifi_policy_init(&s_policy, none, 0);
    return ESP_OK;
}

void wifi_sta_set_networks(const wifi_sta_network_t networks[WIFI_POLICY_NETWORKS_MAX], int preferred)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool present[WIFI_POLICY_NETWORKS_MAX];
    for (int i = 0; i < WIFI_POLICY_NETWORKS_MAX; i++) {
        s_networks[i] = networks[i];
        present[i] = networks[i].ssid[0] != '\0';
    }
    const bool was_up = s_policy.up;
    wifi_policy_init(&s_policy, present, preferred);
    if (s_enabled) {
        if (was_up || s_state == WIFI_STA_CONNECTING) {
            esp_wifi_disconnect();      /* its DISCONNECTED event drives the rejoin */
        } else {
            schedule();
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t wifi_sta_enable(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (on && !s_enabled) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_enabled = true;
            schedule();
        }
    } else if (!on && s_enabled) {
        s_enabled = false;
        esp_timer_stop(s_retry);
        s_policy.up = false;
        s_ip[0] = '\0';
        esp_wifi_disconnect();
        esp_wifi_stop();
        set_state(WIFI_STA_OFF);
    }
    xSemaphoreGive(s_lock);
    return err;
}

wifi_sta_state_t wifi_sta_state(void)
{
    return s_state;
}

const char *wifi_sta_state_name(wifi_sta_state_t state)
{
    switch (state) {
    case WIFI_STA_OFF:        return "off";
    case WIFI_STA_NO_NETWORK: return "no_network";
    case WIFI_STA_CONNECTING: return "connecting";
    case WIFI_STA_CONNECTED:  return "connected";
    }
    return "?";
}

const char *wifi_sta_ssid(void)
{
    const int i = wifi_policy_next(&s_policy);
    return (s_enabled && i >= 0) ? s_networks[i].ssid : NULL;
}

const char *wifi_sta_ip(void)
{
    return s_ip[0] ? s_ip : NULL;
}

int wifi_sta_last_joined(void)
{
    return s_last_joined;
}
