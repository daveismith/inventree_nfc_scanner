#include "net_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_task.h"
#include "net_sync.h"
#include "ota.h"
#include "proto.h"
#include "settings.h"
#include "wifi_sta.h"

#define SYNC_STACK      6144
#define SYNC_PRIO       4
#define RESP_MAX        8192
#define BODY_MAX        8192
#define HTTP_EXTRA_MS   15000           /* beyond the hold asked for */

/* Task notification bits */
#define NOTE_WAKE       (1u << 0)

static const char *TAG = "net";

static SemaphoreHandle_t s_lock;        /* s_ns, s_cfg, s_wifi */
static net_sync_t s_ns;
static settings_net_t s_cfg;
static char s_reader[NET_SYNC_READER_MAX];
static char s_sync_url[APP_NET_URL_MAX + 8];
static wifi_sta_state_t s_wifi = WIFI_STA_OFF;
static TaskHandle_t s_poll_task;
static TaskHandle_t s_report_task;
static volatile bool s_holding;         /* the poll task's request is being held by the server */
static bool s_ever_ok;

/* Each task's own client and buffers; a slow server must not delay the reader. */
typedef struct {
    esp_http_client_handle_t client;
    char body[BODY_MAX];
    char resp[RESP_MAX];
} http_ctx_t;

static http_ctx_t s_poll_http;
static http_ctx_t s_report_http;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void wake(TaskHandle_t task)
{
    if (task) {
        xTaskNotify(task, NOTE_WAKE, eSetBits);
    }
}

/* Under the lock: the URL the calls go to, from the settings. */
static void derive_sync_url(void)
{
    size_t n = (size_t)snprintf(s_sync_url, sizeof(s_sync_url), "%s", s_cfg.url);
    if (n == 0) {
        return;
    }
    while (n > 0 && s_sync_url[n - 1] == '/') {
        s_sync_url[--n] = '\0';
    }
    snprintf(s_sync_url + n, sizeof(s_sync_url) - n, "/sync/");
}

static bool configured(void)
{
    return s_cfg.enabled && s_cfg.url[0] && s_cfg.token[0];
}

/* The link as a sink for app_task: what app_core emits for the plugin. App task context. */
static void link_send(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    lock();
    const bool queued = net_sync_queue(&s_ns, line, len);
    unlock();
    if (queued) {
        wake(s_holding ? s_report_task : s_poll_task);
    }
}

/* A command from the plugin. Under the lock, on a link task. */
static void on_cmd(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    app_task_line(APP_LINK_NET, json, len);
}

/* One call. Returns the HTTP status, or -1 when the server was not reached. */
static int do_call(http_ctx_t *h, size_t body_len, uint32_t timeout_ms, size_t *resp_len)
{
    *resp_len = 0;
    if (h->client == NULL) {
        esp_http_client_config_t cfg = {
            .url = s_sync_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = (int)timeout_ms,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
        };
        h->client = esp_http_client_init(&cfg);
        if (h->client == NULL) {
            return -1;
        }
    }
    char auth[APP_NET_TOKEN_MAX + 8];
    snprintf(auth, sizeof(auth), "Token %s", s_cfg.token);
    esp_http_client_set_url(h->client, s_sync_url);
    esp_http_client_set_method(h->client, HTTP_METHOD_POST);
    esp_http_client_set_timeout_ms(h->client, (int)timeout_ms);
    esp_http_client_set_header(h->client, "Content-Type", "application/json");
    esp_http_client_set_header(h->client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(h->client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot reach %s: %s", s_sync_url, esp_err_to_name(err));
        esp_http_client_cleanup(h->client);
        h->client = NULL;
        return -1;
    }
    bool ok = esp_http_client_write(h->client, h->body, (int)body_len) == (int)body_len
              && esp_http_client_fetch_headers(h->client) >= 0;
    int status = -1;
    if (ok) {
        status = esp_http_client_get_status_code(h->client);
        size_t at = 0;
        for (;;) {
            const int n = esp_http_client_read(h->client, h->resp + at, (int)(sizeof(h->resp) - 1 - at));
            if (n < 0) {
                ok = false;
                break;
            }
            if (n == 0) {
                break;
            }
            at += (size_t)n;
            if (at >= sizeof(h->resp) - 1) {
                break;              /* longer than anything the plugin sends; what fits is parsed */
            }
        }
        h->resp[at] = '\0';
        *resp_len = at;
    }
    esp_http_client_close(h->client);
    if (!ok) {
        esp_http_client_cleanup(h->client);
        h->client = NULL;
        return -1;
    }
    return status;
}

/* Build, call, apply. `hold` asks for the configured hold. */
static void exchange(http_ctx_t *h, bool hold)
{
    lock();
    const size_t body_len = net_sync_request(&s_ns, now_ms(), hold, h->body, sizeof(h->body));
    const uint32_t wait_s = hold ? s_cfg.wait_s : 0;
    unlock();
    if (body_len == 0) {
        ESP_LOGE(TAG, "the request did not fit");
        return;
    }
    if (h == &s_poll_http) {
        s_holding = hold;
    }
    size_t resp_len;
    const int status = do_call(h, body_len, wait_s * 1000 + HTTP_EXTRA_MS, &resp_len);
    if (h == &s_poll_http) {
        s_holding = false;
    }
    lock();
    if (status < 0) {
        net_sync_unreachable(&s_ns, now_ms());
    } else {
        net_sync_response(&s_ns, now_ms(), status, h->resp, resp_len, on_cmd, NULL);
        if (status >= 200 && status < 300) {
            s_ever_ok = true;
        } else {
            ESP_LOGW(TAG, "the plugin answered %d: %.120s", status, h->resp);
        }
    }
    unlock();
    if (status >= 200 && status < 300) {
        ota_note_host_ok();
    }
}

static bool link_ready(void)
{
    return configured() && s_wifi == WIFI_STA_CONNECTED;
}

/* Polls; holds when long polling is on and there is nothing to report. */
static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        lock();
        const bool ready = link_ready();
        const uint32_t delay = ready ? net_sync_delay_ms(&s_ns, now_ms()) : UINT32_MAX;
        const bool hold = s_cfg.wait_s > 0 && !net_sync_has_pending(&s_ns);
        unlock();
        if (delay > 0) {
            uint32_t notes;
            xTaskNotifyWait(0, UINT32_MAX, &notes, delay == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(delay));
            continue;                   /* look again: the wait, or the settings, may have changed */
        }
        exchange(&s_poll_http, hold);
    }
}

/* Reports while the poll task's request is being held, so a tap is not stuck behind it. */
static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t notes;
        xTaskNotifyWait(0, UINT32_MAX, &notes, portMAX_DELAY);
        lock();
        const bool go = link_ready() && s_holding && net_sync_has_pending(&s_ns) && !s_ns.refused;
        unlock();
        if (go) {
            exchange(&s_report_http, false);
        } else {
            wake(s_poll_task);
        }
    }
}

/* Event loop task: signal only. */
static void on_wifi(void *ctx, wifi_sta_state_t state)
{
    (void)ctx;
    s_wifi = state;
    wake(s_poll_task);
    app_task_net_changed();
}

/* Under the lock. */
static void apply_networks(void)
{
    wifi_sta_network_t nets[WIFI_POLICY_NETWORKS_MAX];
    int count = 0;
    for (int i = 0; i < SETTINGS_NETWORKS_MAX; i++) {
        snprintf(nets[i].ssid, sizeof(nets[i].ssid), "%s", s_cfg.networks[i].ssid);
        snprintf(nets[i].psk, sizeof(nets[i].psk), "%s", s_cfg.networks[i].psk);
        count += nets[i].ssid[0] != '\0';
    }
    wifi_sta_set_networks(nets, s_cfg.preferred);
    wifi_sta_enable(s_cfg.enabled && count > 0);
}

void net_link_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);

    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(s_reader, sizeof(s_reader), "nfc-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    settings_net_load(&s_cfg);
    derive_sync_url();
    net_sync_init(&s_ns, s_reader, esp_random() % 1000000 + 1, s_cfg.poll_ms, s_cfg.wait_s);
    app_task_add_link(APP_LINK_NET, true, link_send, NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_sta_init(on_wifi, NULL));

    configASSERT(xTaskCreate(poll_task, "net_poll", SYNC_STACK, NULL, SYNC_PRIO, &s_poll_task) == pdPASS);
    configASSERT(xTaskCreate(report_task, "net_report", SYNC_STACK, NULL, SYNC_PRIO, &s_report_task) == pdPASS);

    lock();
    apply_networks();
    unlock();
    ESP_LOGI(TAG, "reader %s, %s, plugin %s", s_reader, s_cfg.enabled ? "enabled" : "disabled",
             s_cfg.url[0] ? s_cfg.url : "(unset)");
}

void net_link_status(app_net_status_t *out)
{
    static char url[APP_NET_URL_MAX + 1];
    lock();
    snprintf(url, sizeof(url), "%s", s_cfg.url);
    out->enabled = s_cfg.enabled;
    out->wifi = wifi_sta_state_name(s_wifi);
    out->ssid = wifi_sta_ssid();
    out->ip = wifi_sta_ip();
    out->url = url[0] ? url : NULL;
    out->has_token = s_cfg.token[0] != '\0';
    out->reader = s_reader;
    out->link = !configured() ? "off" : (s_wifi != WIFI_STA_CONNECTED ? "no_wifi" : net_sync_link_state(&s_ns));
    out->last_status = s_ns.last_status;
    out->poll_ms = s_cfg.poll_ms;
    out->wait_s = s_cfg.wait_s;
    out->queued = s_ns.count;
    out->dropped = s_ns.dropped;
    unlock();
}

static int find_network(const char *ssid)
{
    for (int i = 0; i < SETTINGS_NETWORKS_MAX; i++) {
        if (strcmp(s_cfg.networks[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

app_err_t net_link_command(const app_cmd_t *cmd, app_net_status_t *status, const char **detail)
{
    app_err_t err = APP_ERR_NONE;
    bool save = false;
    bool networks_changed = false;
    bool server_changed = false;

    lock();
    switch (cmd->net_action) {
    case APP_NET_STATUS:
        break;
    case APP_NET_JOIN: {
        int i = find_network(cmd->ssid);
        if (i < 0) {
            for (int j = 0; j < SETTINGS_NETWORKS_MAX; j++) {
                if (s_cfg.networks[j].ssid[0] == '\0') {
                    i = j;
                    break;
                }
            }
        }
        if (i < 0) {
            *detail = "four networks are already kept; forget one first";
            err = APP_ERR_BAD_ARG;
            break;
        }
        snprintf(s_cfg.networks[i].ssid, sizeof(s_cfg.networks[i].ssid), "%s", cmd->ssid);
        snprintf(s_cfg.networks[i].psk, sizeof(s_cfg.networks[i].psk), "%s", cmd->psk);
        s_cfg.preferred = (uint8_t)i;
        save = networks_changed = true;
        break;
    }
    case APP_NET_FORGET: {
        const int i = find_network(cmd->ssid);
        if (i < 0) {
            *detail = "no such network";
            err = APP_ERR_BAD_ARG;
            break;
        }
        memset(&s_cfg.networks[i], 0, sizeof(s_cfg.networks[i]));
        if (s_cfg.preferred == i) {
            s_cfg.preferred = 0;
        }
        save = networks_changed = true;
        break;
    }
    case APP_NET_SERVER:
#if !CONFIG_APP_NET_ALLOW_HTTP
        if (strncmp(cmd->url, "https://", 8) != 0) {
            *detail = "url: https only (CONFIG_APP_NET_ALLOW_HTTP permits http for testing)";
            err = APP_ERR_BAD_ARG;
            break;
        }
#endif
        snprintf(s_cfg.url, sizeof(s_cfg.url), "%s", cmd->url);
        if (cmd->has_token) {
            snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", cmd->token);
        }
        derive_sync_url();
        save = server_changed = true;
        break;
    case APP_NET_POLL:
        if (cmd->has_poll_ms) {
            s_cfg.poll_ms = cmd->poll_ms;
        }
        if (cmd->has_wait_s) {
            s_cfg.wait_s = cmd->wait_s;
        }
        net_sync_set_pacing(&s_ns, s_cfg.poll_ms, s_cfg.wait_s);
        save = true;
        break;
    case APP_NET_ENABLE:
        break;
    }
    if (err == APP_ERR_NONE && cmd->has_enabled && cmd->enabled != s_cfg.enabled) {
        s_cfg.enabled = cmd->enabled;
        save = networks_changed = true;
    }
    if (err == APP_ERR_NONE && save) {
        settings_net_save(&s_cfg);
    }
    if (err == APP_ERR_NONE && networks_changed) {
        apply_networks();
    }
    if (err == APP_ERR_NONE && (server_changed || networks_changed)) {
        net_sync_reconfigured(&s_ns);
    }
    unlock();

    if (err != APP_ERR_NONE) {
        return err;
    }
    wake(s_poll_task);
    net_link_status(status);
    return APP_ERR_NONE;
}

const char *net_link_url(void)
{
    return s_cfg.url;
}

const char *net_link_token(void)
{
    return s_cfg.token;
}
