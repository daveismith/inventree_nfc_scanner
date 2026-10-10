/*
 * Firmware updates, with a way back.
 *
 * Two ways in: `ota` fetches an image from a URL (network builds), and `ota_begin`,
 * `ota_data` and `ota_end` take one sent over USB, a piece per line. Either way the image is
 * written to the application slot not running, its sha256 checked against the one given, and
 * the device restarts into it. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image is on
 * trial until it confirms itself, which it does here once the reader chip has come up and a
 * host has been reached (USB enumerated for a unit not configured for the plugin, else one
 * `/sync` answered); otherwise the next reset returns to the previous image.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

/* At boot: find out whether this image is on trial, and get ready for updates over USB. */
void ota_init(void);

/* The two things that together confirm an image on trial. From any task. A USB host counts
 * only for a unit not configured to reach a plugin. */
void ota_note_reader_up(void);
void ota_note_host_ok(void);
void ota_note_usb_host(void);
/* From the network link once its settings are loaded: a USB host seen before then is judged now. */
void ota_net_settings_known(void);

/* For app_core's environment, from the app task: `ota`, `ota_begin`, `ota_data`, `ota_end`.
 * Progress comes as `ota` events. */
app_err_t ota_command(const app_cmd_t *cmd, const char **detail);

/* From the app task: a link went down (an update it was sending ends), and now and then (an
 * update sent over USB that has gone quiet ends). */
void ota_link_down(uint8_t origin);
void ota_poll(void);

/* An update is under way, by either route: no job may begin, and no second update. */
bool ota_in_progress(void);

/* Shared with the network part (ota_net.c). */
/* An `ota` event. `error` says why an update failed, for a program; `detail`, for a person. */
void ota_announce(const char *state, app_err_t error, const char *detail);
void ota_restart_when_free(void);
bool ota_net_claim(void);
void ota_net_release(void);
app_err_t ota_net_start(const app_cmd_t *cmd, const char **detail);
