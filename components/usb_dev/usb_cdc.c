#include "usb_dev.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#if CONFIG_TINYUSB_HID_COUNT > 0
#include "usb_descriptors.h"
esp_err_t usb_hid_init(void);
#endif

#define ITF             TINYUSB_CDC_ACM_0
#define RX_STREAM_SIZE  (2 * USB_CDC_LINE_MAX)
#define RX_TASK_STACK   4096
#define RX_TASK_PRIO    4

static const char *TAG = "usb_dev";

static usb_dev_config_t s_cfg;
static StreamBufferHandle_t s_rx;
static SemaphoreHandle_t s_tx_lock;
static volatile bool s_mounted;
static volatile size_t s_rx_accepted;              /* bytes put into the stream so far */
/* Where bytes were lost, as positions in the stream: written by the TinyUSB task, read by the
 * receive task, each with its own index, so no update is ever missed. */
#define LOSS_RING   4
static volatile size_t s_loss_pos[LOSS_RING];
static volatile unsigned s_loss_w, s_loss_r;
static bool s_dtr;
static bool s_touch_armed;

/* TinyUSB task: move what arrived into the stream buffer and return. */
static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t n = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) == ESP_OK && n > 0) {
        const size_t took = xStreamBufferSend(s_rx, buf, n, 0);
        s_rx_accepted += took;
        if (took != n) {
            /* The line in progress at this byte is damaged, through its end. */
            if (s_loss_w - s_loss_r < LOSS_RING) {
                s_loss_pos[s_loss_w % LOSS_RING] = s_rx_accepted;
                s_loss_w++;
            } else {
                s_loss_pos[(s_loss_w - 1) % LOSS_RING] = s_rx_accepted;   /* the latest stands for the rest */
            }
        }
    }
}

static void set_link(bool up)
{
    if (up == s_dtr) {
        return;
    }
    s_dtr = up;
    if (s_cfg.on_link) {
        s_cfg.on_link(s_cfg.ctx, up);
    }
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    const bool dtr = event->line_state_changed_data.dtr;
    const bool closing = s_dtr && !dtr;
    set_link(dtr);
    if (closing && s_touch_armed) {
        s_touch_armed = false;
        ESP_LOGW(TAG, "1200 baud touch");
        if (s_cfg.on_touch_1200) {
            s_cfg.on_touch_1200(s_cfg.ctx);
        }
    }
}

static void cdc_line_coding_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_touch_armed = event->line_coding_changed_data.p_line_coding->bit_rate == 1200;
}

static void device_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_mounted = true;
        if (s_cfg.on_mount) {
            s_cfg.on_mount(s_cfg.ctx, true);
        }
        break;
    case TINYUSB_EVENT_DETACHED:
        s_mounted = false;
        set_link(false);                /* a pulled cable never lowers DTR */
        if (s_cfg.on_mount) {
            s_cfg.on_mount(s_cfg.ctx, false);
        }
        break;
    default:
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static char line[USB_CDC_LINE_MAX];
    size_t len = 0;
    bool discard = false;
    size_t consumed = 0;                /* bytes taken from the stream so far */

    for (;;) {
        uint8_t chunk[128];
        const size_t n = xStreamBufferReceive(s_rx, chunk, sizeof(chunk), portMAX_DELAY);
        for (size_t i = 0; i < n; i++) {
            while (s_loss_r != s_loss_w && consumed == s_loss_pos[s_loss_r % LOSS_RING]) {
                discard = true;         /* from here to the end of this line is what the loss cut */
                s_loss_r++;
            }
            consumed++;
            const char c = (char)chunk[i];
            if (c == '\n') {
                if (discard) {
                    discard = false;
                    if (s_cfg.on_line_too_long) {
                        s_cfg.on_line_too_long(s_cfg.ctx);
                    }
                } else {
                    if (len > 0 && line[len - 1] == '\r') {
                        len--;
                    }
                    line[len] = '\0';
                    if (len > 0 && s_cfg.on_line) {
                        s_cfg.on_line(s_cfg.ctx, line, len);
                    }
                }
                len = 0;
            } else if (!discard) {
                if (len + 1 >= sizeof(line)) {
                    discard = true;     /* skip to the end of this line, then say so */
                    len = 0;
                } else {
                    line[len++] = c;
                }
            }
        }
    }
}

esp_err_t usb_dev_init(const usb_dev_config_t *config)
{
    s_cfg = *config;
    s_rx = xStreamBufferCreate(RX_STREAM_SIZE, 1);
    s_tx_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rx && s_tx_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_FALSE(xTaskCreate(rx_task, "cdc_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "cannot create the receive task");

    /* For CDC alone esp_tinyusb builds the descriptors itself; the VID/PID and strings come
     * from sdkconfig.defaults. With the keyboard, usb_descriptors.c supplies them. */
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(device_event_cb);
#if CONFIG_TINYUSB_HID_COUNT > 0
    usb_descriptors_fill(&tusb_cfg.descriptor);
    ESP_RETURN_ON_ERROR(usb_hid_init(), TAG, "usb_hid_init");
#endif
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = ITF,
        .callback_rx = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
        .callback_line_coding_changed = cdc_line_coding_cb,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), TAG, "tinyusb_cdcacm_init");
    return ESP_OK;
}

bool usb_dev_mounted(void)
{
    return s_mounted;
}

bool usb_cdc_connected(void)
{
    return s_mounted && tud_cdc_n_connected(ITF);
}

bool usb_cdc_send(const char *data, size_t len, uint32_t wait_ms)
{
    if (s_tx_lock == NULL || !usb_cdc_connected()) {
        return false;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t wait = pdMS_TO_TICKS(wait_ms);
    if (xSemaphoreTake(s_tx_lock, wait) != pdTRUE) {
        return false;
    }

    bool sent = false;
    for (;;) {
        if (tud_cdc_n_write_available(ITF) >= len) {
            tinyusb_cdcacm_write_queue(ITF, (const uint8_t *)data, len);
            tinyusb_cdcacm_write_flush(ITF, 0);
            sent = true;
            break;
        }
        if (xTaskGetTickCount() - start >= wait || !usb_cdc_connected()) {
            break;
        }
        tinyusb_cdcacm_write_flush(ITF, 0);
        vTaskDelay(1);
    }
    xSemaphoreGive(s_tx_lock);
    return sent;
}

void usb_dev_disconnect(void)
{
    tud_disconnect();
}

#if !(CONFIG_TINYUSB_HID_COUNT > 0)
bool usb_hid_available(void)
{
    return false;
}

bool usb_hid_type(const char *text)
{
    (void)text;
    return false;
}
#endif
