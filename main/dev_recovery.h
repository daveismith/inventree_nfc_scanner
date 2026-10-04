/*
 * A way back for a board nobody can reach.
 *
 * Firmware that crashes at boot, or never appears on USB, cannot be asked to enter download
 * mode, and without someone at the BOOT button that is the end of the session. While
 * CONFIG_APP_DEV_RECOVERY is on, the firmware sends itself there instead:
 *
 *   - after CONFIG_APP_DEV_RECOVERY_BOOT_LOOPS resets in a row caused by a panic or a
 *     watchdog, none of which stayed up CONFIG_APP_DEV_RECOVERY_HEALTHY_S seconds;
 *   - when no host has enumerated it CONFIG_APP_DEV_RECOVERY_MOUNT_TIMEOUT_S seconds after boot.
 *
 * Both act only after USB is up, through app_download_mode_request(), so the path taken is
 * the one that is exercised on every ordinary flash.
 *
 * Turn it off for a device in use: one powered from a charger, with no host to enumerate it,
 * would otherwise sit in download mode.
 */
#pragma once

#include <stdbool.h>

#include "app_types.h"

/* Count this boot. Call early, before anything that might crash. */
void dev_recovery_init(void);

/* True when the last restart asked for this boot to stay off the bus (the guard's self-test). */
bool dev_recovery_stay_off_usb(void);

/* USB is installed: act on a boot loop, and start watching for enumeration. Returns true when
 * download mode has been requested, in which case the caller should start nothing more. */
bool dev_recovery_usb_started(void);

/* From the USB layer, on its task: a host enumerated the device. */
void dev_recovery_usb_mounted(void);

/* Whether the `debug` command exists in this build. */
bool dev_recovery_debug_available(void);

/* Provoke a failure on purpose, to prove the guards. May not return. Task context. */
void dev_recovery_debug(app_debug_action_t action);
