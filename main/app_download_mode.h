#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the shutdown handler that arms download mode, and create the task that performs
 * the reboot. Call once from app_main(), before anything that might ask for it.
 */
void app_download_mode_init(void);

/*
 * Ask for a restart into the ROM's download mode on USB-Serial-JTAG (303a:1001), where plain
 * esptool works and resets the board back into the app when it is done.
 *
 * This only signals the reboot task, so it is safe from any task, including the TinyUSB task
 * and esp_timer callbacks. The restart follows about 150 ms later.
 */
void app_download_mode_request(void);

#ifdef __cplusplus
}
#endif
