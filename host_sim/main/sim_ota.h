/* Updates sent over the serial link, simulated: the image goes into memory, its sha256 is
 * checked, and a good one "restarts" the simulator under a new version. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

typedef void (*sim_ota_restart_fn)(const char *new_fw);

void sim_ota_init(sim_ota_restart_fn restart);
app_err_t sim_ota_command(const app_cmd_t *cmd, uint32_t now_ms, const char **detail);
bool sim_ota_active(void);
void sim_ota_poll(uint32_t now_ms);
/* The version the simulator runs: SIM_FW, or what the last update installed. */
const char *sim_ota_fw(void);
