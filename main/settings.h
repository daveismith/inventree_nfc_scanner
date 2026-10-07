/* The few settings the device keeps across power cycles, in NVS. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#define SETTINGS_NETWORKS_MAX   4

typedef struct {
    char ssid[APP_NET_SSID_MAX + 1];
    char psk[APP_NET_PSK_MAX + 1];
} settings_network_t;

#define SETTINGS_NET_VERSION    1

/* Everything the network link is configured with. The token is read only by the link. Stored
 * as one blob; `version` and `size` say whether a stored one is this layout. */
typedef struct {
    uint16_t version;
    uint16_t size;
    bool enabled;
    char url[APP_NET_URL_MAX + 1];      /* the plugin's URL, "" when unset */
    char token[APP_NET_TOKEN_MAX + 1];  /* "" when unset */
    uint32_t poll_ms;
    uint32_t wait_s;
    uint8_t preferred;                  /* the network that last worked, to try first */
    settings_network_t networks[SETTINGS_NETWORKS_MAX];     /* unused entries have an empty ssid */
} settings_net_t;

/* Open the store. Erases and starts again if what is in the partition cannot be read. */
void settings_init(void);

/* Whether a tap types its text record when no session has said otherwise. */
bool settings_hid_default(void);
void settings_set_hid_default(bool enabled);

/* The network settings, with the build's defaults where nothing is stored. */
void settings_net_load(settings_net_t *out);
void settings_net_save(const settings_net_t *net);
