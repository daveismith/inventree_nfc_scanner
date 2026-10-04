/*
 * The keyboard: types a tag's text record, then Enter, into whatever has focus on the host.
 *
 * Strings are queued and typed on a task of their own, one key press and release at a time,
 * each sent only once the host has collected the one before.
 */
#include "usb_dev.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "usb_descriptors.h"

#define TEXT_MAX        128
#define QUEUE_LEN       2
#define TASK_STACK      3072
#define TASK_PRIO       4
#define REPORT_WAIT_MS  100         /* the endpoint is polled every 10 ms */

/* ASCII to a US-layout key and whether it needs shift. */
static const uint8_t s_keymap[128][2] = { HID_ASCII_TO_KEYCODE };

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_sent;

/* TinyUSB's HID callbacks. The three without a weak default must exist. */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return usb_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    /* The host's lock-key LEDs; nothing here shows them. */
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
    xSemaphoreGive(s_sent);
}

static bool usable(void)
{
    return tud_mounted() && !tud_suspended();
}

/* One report, delivered. False when the host is not collecting them. */
static bool send_report(uint8_t modifier, uint8_t keycode)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(REPORT_WAIT_MS);
    while (!tud_hid_ready()) {
        if (!usable() || xTaskGetTickCount() >= deadline) {
            return false;
        }
        vTaskDelay(1);
    }
    uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
    xSemaphoreTake(s_sent, 0);                  /* forget a completion nobody waited for */
    if (!tud_hid_keyboard_report(0, modifier, keys)) {
        return false;
    }
    return xSemaphoreTake(s_sent, pdMS_TO_TICKS(REPORT_WAIT_MS)) == pdTRUE;
}

static bool type_key(uint8_t modifier, uint8_t keycode)
{
    /* Released between keys, so a repeated letter is two presses and not one held key. */
    return send_report(modifier, keycode) && send_report(0, 0);
}

static void hid_task(void *arg)
{
    (void)arg;
    static char text[TEXT_MAX];
    for (;;) {
        xQueueReceive(s_queue, text, portMAX_DELAY);
        if (!usable()) {
            continue;
        }
        bool ok = true;
        for (const char *p = text; *p && ok; p++) {
            const uint8_t c = (uint8_t)*p;
            if (c >= 128 || s_keymap[c][1] == 0) {
                continue;
            }
            ok = type_key(s_keymap[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0, s_keymap[c][1]);
        }
        if (ok) {
            ok = type_key(0, HID_KEY_ENTER);
        }
        if (!ok) {
            send_report(0, 0);                  /* never leave a key held down */
        }
    }
}

esp_err_t usb_hid_init(void)
{
    s_queue = xQueueCreate(QUEUE_LEN, TEXT_MAX);
    s_sent = xSemaphoreCreateBinary();
    if (s_queue == NULL || s_sent == NULL
            || xTaskCreate(hid_task, "hid_kbd", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool usb_hid_available(void)
{
    return s_queue != NULL;
}

bool usb_hid_type(const char *text)
{
    if (s_queue == NULL || strlen(text) >= TEXT_MAX || !usable()) {
        return false;
    }
    char item[TEXT_MAX] = { 0 };
    strlcpy(item, text, sizeof(item));
    return xQueueSend(s_queue, item, 0) == pdTRUE;
}
