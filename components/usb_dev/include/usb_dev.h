/*
 * The device's USB side: TinyUSB, and the CDC-ACM interface as a line-oriented link.
 *
 * Received bytes are cut into lines on a task of their own and handed to on_line. Sending is
 * a whole line or nothing: a line that cannot be queued in full is dropped, so the far end
 * never sees half a JSON object.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_CDC_LINE_MAX 2048       /* longest line accepted, terminator included */

typedef struct {
    void *ctx;

    /* On the receive task. `line` is NUL terminated, without its line ending; never empty. */
    void (*on_line)(void *ctx, const char *line, size_t len);
    /* On the receive task: a line was longer than USB_CDC_LINE_MAX, or bytes were lost. */
    void (*on_line_too_long)(void *ctx);

    /* The three below run on the TinyUSB task and must only signal, never block or restart. */

    /* A host opened (DTR raised) or closed the port. */
    void (*on_link)(void *ctx, bool up);
    /* The device was enumerated by a host, or left the bus. */
    void (*on_mount)(void *ctx, bool mounted);
    /* The port was opened at 1200 baud and closed: the conventional "enter the bootloader". */
    void (*on_touch_1200)(void *ctx);
} usb_dev_config_t;

esp_err_t usb_dev_init(const usb_dev_config_t *config);

/* Enumerated by a host. */
bool usb_dev_mounted(void);

/* A host has the CDC port open. */
bool usb_cdc_connected(void);

/*
 * Queue `len` bytes, waiting up to `wait_ms` for room. Returns false, having sent none of it,
 * when no host has the port open or there was no room in time. Safe from any task.
 */
bool usb_cdc_send(const char *data, size_t len, uint32_t wait_ms);

/* Drop off the bus (D+ pull-up released): before restarting into download mode, and for the
 * recovery guard's self-test. */
void usb_dev_disconnect(void);

/* The keyboard interface is part of this build (CONFIG_TINYUSB_HID_COUNT). */
bool usb_hid_available(void);

/*
 * Type `text` and Enter on the host, as a US-layout keyboard. Queued, and typed in the
 * background. Returns false when there is no keyboard, the host is not listening, or two
 * strings are already waiting. Characters outside printable ASCII are skipped.
 */
bool usb_hid_type(const char *text);

#ifdef __cplusplus
}
#endif
