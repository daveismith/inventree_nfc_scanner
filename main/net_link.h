/*
 * The plugin as a host: the network link.
 *
 * Joins Wi-Fi (components/wifi_sta), and speaks the plugin's `/sync` exchange
 * (components/net_sync) over esp_http_client on two tasks of its own: one that polls, holding
 * a request when long polling is on, and one that reports what happened while the other is
 * being held. Commands from the plugin enter app_task as link APP_LINK_NET, remote; what
 * app_core emits for that link is queued here for the next call. The `net` command, from
 * USB only, changes the settings, which live in NVS (main/settings.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_types.h"

/* After settings_init() and app_task_init(). Starts the radio if the settings say so. */
void net_link_init(void);

/* For app_core's environment. */
app_err_t net_link_command(const app_cmd_t *cmd, app_net_status_t *status, const char **detail);

/* What `info` reports. The strings live until the next call. */
void net_link_status(app_net_status_t *out);

/* The plugin's URL as configured, "" when unset. For the updater, which may need the token. */
const char *net_link_url(void);
const char *net_link_token(void);
