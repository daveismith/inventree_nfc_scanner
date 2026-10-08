/*
 * Updates over the network, with a way back.
 *
 * `ota` fetches an image from a URL into the application slot not running, checks it, and
 * restarts into it. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image is on trial
 * until it confirms itself, which it does here once the reader chip has come up and a host
 * has been reached (USB enumerated, or one `/sync` answered); otherwise the next reset
 * returns to the previous image.
 */
#pragma once

#include <stdbool.h>

#include "app_types.h"

/* At boot: find out whether this image is on trial. */
void ota_init(void);

/* The two things that together confirm an image on trial. From any task. A USB host counts
 * only for a unit not configured to reach a plugin. */
void ota_note_reader_up(void);
void ota_note_host_ok(void);
void ota_note_usb_host(void);

/* For app_core's environment: start an update. Progress comes as `ota` events. From a remote
 * link the image must be on the plugin's own origin; the digest is always required. */
app_err_t ota_start(const app_cmd_t *cmd, const char **detail);

bool ota_in_progress(void);
