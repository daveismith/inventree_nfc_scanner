/*
 * A Wi-Fi station that keeps itself joined.
 *
 * Give it up to four networks and it joins the preferred one, tries the others when that
 * fails, and reconnects with back-off whenever the link drops (wifi_policy.h has the rules).
 * It owns the radio: nothing else in the firmware calls esp_wifi. Changes of state come
 * through one callback, on the default event loop's task; it must only signal.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STA_OFF,                       /* the radio is off */
    WIFI_STA_NO_NETWORK,                /* on, with nothing to join */
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,                 /* with an address */
} wifi_sta_state_t;

typedef struct {
    char ssid[33];
    char psk[65];                       /* "" for an open network */
} wifi_sta_network_t;

typedef void (*wifi_sta_on_change_t)(void *ctx, wifi_sta_state_t state);

/* Set up the radio, off. Needs the default event loop and esp_netif to exist. */
esp_err_t wifi_sta_init(wifi_sta_on_change_t on_change, void *ctx);

/* The networks to use, `preferred` first. Takes effect at once: a station that is on
 * rejoins. Entries with an empty ssid are ignored. */
void wifi_sta_set_networks(const wifi_sta_network_t networks[WIFI_POLICY_NETWORKS_MAX], int preferred);

/* On the air, or off it. */
esp_err_t wifi_sta_enable(bool on);

wifi_sta_state_t wifi_sta_state(void);
const char *wifi_sta_state_name(wifi_sta_state_t state);

/* The network joined or being joined (NULL when none), and the address when connected. */
const char *wifi_sta_ssid(void);
const char *wifi_sta_ip(void);

/* Which network last joined, for the caller to remember as preferred; -1 for none yet. */
int wifi_sta_last_joined(void);

#ifdef __cplusplus
}
#endif
