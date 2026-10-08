/*
 * The one task that owns the device's state: it runs app_core, fed by command lines from the
 * links, and sends what app_core emits back down them.
 *
 * A link is a host: the USB page, or the plugin over the network. Each has a number, which
 * is the `origin` of the commands it sends; an answer goes back to the link that asked and
 * an event to every link. A remote link is one with nobody at the board; some commands are
 * refused from it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_LINK_USB    0
#define APP_LINK_NET    1
#define APP_LINK_MAX    2

/* Called with each line for the link, '\n' included. Must not block for long: the app task
 * is the one calling. */
typedef void (*app_link_send_t)(void *ctx, const char *line, size_t len);

/* Create the queue and the state machine. Before USB starts, so its callbacks have somewhere
 * to deliver to. */
void app_task_init(void);

void app_task_start(void);

/* Where a link's lines go. Before the link sends its first command. */
void app_task_add_link(uint8_t origin, bool remote, app_link_send_t send, void *ctx);

/* Whether a job is running. From any task; a snapshot. */
bool app_task_job_active(void);

/* A line already formatted, to one link or (APP_ORIGIN_ALL) every link. From any task. */
void app_task_send(uint8_t origin, const char *line, size_t len);

/* From the network link, any task: the Wi-Fi or plugin link changed state; USB is told. */
void app_task_net_changed(void);

/* From a link's receive task: one line from its host. */
void app_task_line(uint8_t origin, const char *line, size_t len);
void app_task_line_too_long(uint8_t origin);

/* From the TinyUSB task: a host opened or closed the port. Only the USB link has sessions. */
void app_task_link(uint8_t origin, bool up);
