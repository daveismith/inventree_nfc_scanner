/*
 * ESP_LOG output, over the protocol.
 *
 * The CDC port carries one JSON object a line and nothing else, so log text never goes there
 * raw. Logs go to the UART console as always; when a host asks with {"cmd":"log",...}, each
 * line at or above that level is also sent as {"evt":"log","lvl":"W","tag":"...","msg":"..."}.
 * It is best effort: a line is dropped rather than ever making the logging task wait.
 */
#pragma once

#include "app_types.h"

void log_forward_init(void);

/* APP_LOG_OFF stops forwarding. Debug also raises the runtime log level to debug. */
void log_forward_set_level(app_log_level_t level);
