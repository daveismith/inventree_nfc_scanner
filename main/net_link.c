#include "net_link.h"

#include <stdio.h>
#include <stdlib.h>
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

#define SYNC_STACK      8192            /* a TLS handshake with the certificate bundle, and cJSON */
#define SYNC_PRIO       4
#define RESP_MAX        8192
#define BODY_MAX        8192
#define HTTP_EXTRA_MS   15000           /* beyond the hold asked for */
#define RETRY_IF_FAILED_WITHIN_MS 3000  /* a kept connection found dead fails fast; a timeout does not */

/* Task notification bits */
#define NOTE_WAKE       (1u << 0)

static const char *TAG = "net";

static SemaphoreHandle_t s_lock;        /* s_ns, s_cfg, s_wifi */
static SemaphoreHandle_t s_dispatch;    /* one answer is applied and its commands handed on at a time, in order */
static net_sync_t s_ns;
static settings_net_t s_cfg;
static char s_reader[NET_SYNC_READER_MAX];
static char s_sync_url[APP_NET_URL_MAX + 8];
static wifi_sta_state_t s_wifi = WIFI_STA_OFF;
static TaskHandle_t s_poll_task;
static TaskHandle_t s_report_task;
static volatile bool s_holding;         /* the poll task's request is being held by the server */
static bool s_ever_ok;
static volatile bool s_configured_flag;     /* configured(), for callers that must not take the lock */

/* Each task's own client and buffers; a slow server must not delay the reader. */
typedef struct {
    esp_http_client_handle_t client;
    char body[BODY_MAX];
    char resp[RESP_MAX];
    /* Taken in the same critical section as the body, so a call built for one server never
     * goes to another with the other's token. Wiped once the call is over. */
    char url[APP_NET_URL_MAX + 8];
    char auth[APP_NET_TOKEN_MAX + 8];
} http_ctx_t;

/*
 * Commands from the plugin are gathered here while the lock is held and handed to the app
 * task after it is released: app_task_line() answers a line it cannot parse through
 * link_send(), which takes the same lock, and may wait on the command queue, which the app
 * task cannot drain while it waits for the lock to emit. One is enough for a call; a server
 * with more queued sends the rest with the next call.
 */
typedef struct {
    int count;
    char json[2][NET_SYNC_CMD_MAX];
} cmd_batch_t;

static http_ctx_t s_poll_http;
static http_ctx_t s_report_http;
static cmd_batch_t s_poll_batch;
static cmd_batch_t s_report_batch;

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

bool url_allowed(const char *url)
{
    const bool https = strncmp(url, "https://", 8) == 0;
#if CONFIG_APP_NET_ALLOW_HTTP
    const bool scheme_ok = https || strncmp(url, "http://", 7) == 0;
#else
    const bool scheme_ok = https;
#endif
    if (!scheme_ok) {
        return false;
    }
    /* No user info: "https://host@evil/" would send the token where the eye does not look. */
    const char *rest = url + (https ? 8 : 7);
    const char *slash = strchr(rest, '/');
    const char *at = strchr(rest, '@');
    return rest[0] != '\0' && (at == NULL || (slash != NULL && at > slash));
}

/* The scheme, host and port of a URL: `*len` bytes from its start. */
/*
 * A URL's origin as "scheme://host:port", the port written out even when it was left to
 * the default: the HTTP client reports the URL it connected to that way, and
 * "https://host" and "https://host:443" are one origin. Returns false for anything that is
 * not an http(s) URL.
 */
static bool canonical_origin(const char *url, char *out, size_t cap)
{
    const bool https = strncmp(url, "https://", 8) == 0;
    const char *rest = https ? url + 8 : (strncmp(url, "http://", 7) == 0 ? url + 7 : NULL);
    if (rest == NULL || rest[0] == '\0' || rest[0] == '/') {
        return false;
    }
    const char *slash = strchr(rest, '/');
    const size_t authority_len = slash ? (size_t)(slash - rest) : strlen(rest);
    /* The port, if given: after the last ':' that follows any ']' of an IPv6 literal. */
    const char *bracket = memchr(rest, ']', authority_len);
    const char *colon = NULL;
    for (const char *p = bracket ? bracket : rest; p < rest + authority_len; p++) {
        if (*p == ':') {
            colon = p;
        }
    }
    const size_t host_len = colon ? (size_t)(colon - rest) : authority_len;
    const int port = colon ? atoi(colon + 1) : (https ? 443 : 80);
    if (host_len == 0 || port <= 0 || port > 65535) {
        return false;
    }
    const int n = snprintf(out, cap, "%s://%.*s:%d", https ? "https" : "http", (int)host_len, rest, port);
    return n > 0 && (size_t)n < cap;
}

bool same_origin(const char *a, const char *b)
{
    char oa[APP_OTA_URL_MAX + 1], ob[APP_OTA_URL_MAX + 1];
    return canonical_origin(a, oa, sizeof(oa)) && canonical_origin(b, ob, sizeof(ob)) && strcmp(oa, ob) == 0;
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
    return s_cfg.enabled && s_cfg.url[0] && s_cfg.token[0] && url_allowed(s_cfg.url);
}

bool net_link_configured(void)
{
    return s_configured_flag;
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

static bool on_cmd(void *ctx, const char *json, size_t len)
{
    cmd_batch_t *batch = ctx;
    if (batch->count == 2) {
        return false;
    }
    memcpy(batch->json[batch->count], json, len + 1);
    batch->count++;
    return true;
}

/* One attempt at a call. Returns the HTTP status, or -1 when the server was not reached;
 * *fresh says whether the attempt began on a new connection. */
static int do_call_once(http_ctx_t *h, size_t body_len, uint32_t timeout_ms, size_t *resp_len, bool *fresh)
{
    *resp_len = 0;
    *fresh = h->client == NULL;
    if (h->client == NULL) {
        esp_http_client_config_t cfg = {
            .url = h->url,
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
    esp_http_client_set_url(h->client, h->url);
    esp_http_client_set_method(h->client, HTTP_METHOD_POST);
    esp_http_client_set_timeout_ms(h->client, (int)timeout_ms);
    esp_http_client_set_header(h->client, "Content-Type", "application/json");
    esp_http_client_set_header(h->client, "Authorization", h->auth);

    esp_err_t err = esp_http_client_open(h->client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot reach %s: %s", h->url, esp_err_to_name(err));
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
                /* Longer than anything the plugin sends. What fits is parsed; the rest would
                 * sit on the connection and be read as the next answer, so it is dropped. */
                ESP_LOGW(TAG, "an answer over %u bytes was cut", (unsigned)sizeof(h->resp));
                esp_http_client_close(h->client);
                esp_http_client_cleanup(h->client);
                h->client = NULL;
                break;
            }
        }
        h->resp[at] = '\0';
        *resp_len = at;
    }
    if (!ok) {
        esp_http_client_cleanup(h->client);
        h->client = NULL;
        return -1;
    }
    /* Not closed: with the body read to its end the connection stays open for the next call,
     * which spares a TLS handshake a second under plain polling. */
    return status;
}

/* One call. A connection kept from the last call may have been closed by the server or a
 * proxy meanwhile; that failure is tried again at once on a fresh one, and only a fresh
 * connection's failure means the server is out of reach. */
static int do_call(http_ctx_t *h, size_t body_len, uint32_t timeout_ms, size_t *resp_len)
{
    bool fresh;
    const uint32_t started = now_ms();
    int status = do_call_once(h, body_len, timeout_ms, resp_len, &fresh);
    /* A connection the server had dropped fails at once; one that timed out did not, and
     * trying again would only double the wait. */
    if (status < 0 && !fresh && now_ms() - started < RETRY_IF_FAILED_WITHIN_MS) {
        ESP_LOGI(TAG, "the kept connection was gone; connecting again");
        status = do_call_once(h, body_len, timeout_ms, resp_len, &fresh);
    }
    return status;
}

/* Build, call, apply. `hold` asks for the configured hold. */
static void exchange(http_ctx_t *h, bool hold)
{
    lock();
    const size_t body_len = net_sync_request(&s_ns, now_ms(), hold, h->body, sizeof(h->body));
    const uint32_t wait_s = hold ? s_cfg.wait_s : 0;
    const uint32_t generation = net_sync_generation(&s_ns);
    snprintf(h->url, sizeof(h->url), "%s", s_sync_url);
    snprintf(h->auth, sizeof(h->auth), "Token %s", s_cfg.token);
    if (h == &s_poll_http) {
        s_holding = hold;           /* under the lock, so link_send cannot see it stale */
    }
    unlock();
    if (body_len == 0) {
        ESP_LOGE(TAG, "the request did not fit");   /* cannot happen: an empty call always fits */
        s_holding = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    size_t resp_len;
    const int status = do_call(h, body_len, wait_s * 1000 + HTTP_EXTRA_MS, &resp_len);
    memset(h->auth, 0, sizeof(h->auth));    /* the client keeps its own copy of the header */
    if (h == &s_poll_http) {
        s_holding = false;
    }
    cmd_batch_t *batch = h == &s_poll_http ? &s_poll_batch : &s_report_batch;
    batch->count = 0;
    /* Applied and handed on one answer at a time, so commands reach the app task in the
     * order the server numbered them even when both tasks have an answer. */
    xSemaphoreTake(s_dispatch, portMAX_DELAY);
    lock();
    if (net_sync_generation(&s_ns) != generation) {
        ESP_LOGI(TAG, "an answer from the previous server, dropped");
        unlock();
        xSemaphoreGive(s_dispatch);
        return;
    }
    if (status < 0) {
        net_sync_unreachable(&s_ns, now_ms());
    } else {
        net_sync_response(&s_ns, now_ms(), status, h->resp, resp_len, on_cmd, batch);
        if (status >= 200 && status < 300) {
            s_ever_ok = true;
        } else {
            ESP_LOGW(TAG, "the plugin answered %d: %.120s", status, h->resp);
        }
    }
    unlock();
    for (int i = 0; i < batch->count; i++) {
        app_task_line(APP_LINK_NET, batch->json[i], strlen(batch->json[i]));
    }
    xSemaphoreGive(s_dispatch);
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
    memset(nets, 0, sizeof(nets));      /* the passphrases: the station has its own copy */
}

void net_link_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_dispatch = xSemaphoreCreateMutex();
    configASSERT(s_lock && s_dispatch);

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
    s_configured_flag = configured();
    unlock();
    ESP_LOGI(TAG, "reader %s, %s, plugin %s", s_reader, s_cfg.enabled ? "enabled" : "disabled",
             s_cfg.url[0] ? s_cfg.url : "(unset)");
    if (s_cfg.url[0] && !url_allowed(s_cfg.url)) {
        ESP_LOGW(TAG, "the stored plugin URL is not allowed by this build (http); the link stays off");
    }
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
        if (!url_allowed(cmd->url)) {
            *detail = "url: https only, with no user info (CONFIG_APP_NET_ALLOW_HTTP permits http for testing)";
            err = APP_ERR_BAD_ARG;
            break;
        }
        if (!same_origin(cmd->url, s_cfg.url) && !cmd->has_token) {
            /* The token was issued for the old server; it is not sent to a new one. */
            memset(s_cfg.token, 0, sizeof(s_cfg.token));
        }
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
    }
    if (err == APP_ERR_NONE && cmd->has_enabled && cmd->enabled != s_cfg.enabled) {
        s_cfg.enabled = cmd->enabled;
        save = networks_changed = true;
    }
    if (err == APP_ERR_NONE && save) {
        settings_net_save(&s_cfg);
        s_configured_flag = configured();
    }
    if (err == APP_ERR_NONE && networks_changed) {
        apply_networks();
    }
    if (err == APP_ERR_NONE && server_changed) {
        net_sync_new_server(&s_ns);
    } else if (err == APP_ERR_NONE && networks_changed) {
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

void net_link_server(char *url, size_t url_cap, char *token, size_t token_cap)
{
    lock();
    snprintf(url, url_cap, "%s", s_cfg.url);
    snprintf(token, token_cap, "%s", s_cfg.token);
    unlock();
}
