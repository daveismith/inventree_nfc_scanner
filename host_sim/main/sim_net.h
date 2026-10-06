/*
 * The simulator's network link: the real net_sync exchange over a plain HTTP socket, driven
 * from the main loop without blocking it. Configured by environment variables:
 *
 *   SIM_SYNC_URL   the plugin's URL, http only, e.g. http://127.0.0.1:8765 (nothing: no link)
 *   SIM_TOKEN      sent as "Authorization: Token ..."
 *   SIM_READER     reader id (default nfc-sim000000)
 *   SIM_WAIT_S     hold asked for (default 0: plain polling)
 *   SIM_POLL_MS    idle interval (default 1000)
 *   SIM_BOOT       the boot number (default: random)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_types.h"

bool sim_net_configured(void);
void sim_net_init(void);

/* A line the state machine emitted for the network link. */
void sim_net_queue(const char *line, size_t len);

/* Move the exchange on. Returns a command to run, as a line, when one has arrived. */
bool sim_net_step(char *cmd_out, size_t cap);

void sim_net_status(app_net_status_t *out);
