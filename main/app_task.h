/*
 * The one task that owns the device's state: it runs app_core, fed by command lines from the
 * USB link, and sends what app_core emits back down it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Create the queue and the state machine. Before USB starts, so its callbacks have somewhere
 * to deliver to. */
void app_task_init(void);

void app_task_start(void);

/* From the USB receive task: one line from the host. */
void app_task_line(const char *line, size_t len);
void app_task_line_too_long(void);

/* From the TinyUSB task: a host opened or closed the port. */
void app_task_link(bool up);
