#include "log_forward.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "proto.h"
#include "usb_dev.h"

static vprintf_like_t s_console;
static atomic_int s_level = APP_LOG_OFF;
static atomic_flag s_busy = ATOMIC_FLAG_INIT;

/* Guarded by s_busy: one line is formatted at a time, and a second logger just skips. */
static char s_raw[320];
static char s_line[480];

static app_log_level_t level_of(char letter)
{
    switch (letter) {
    case 'E': return APP_LOG_ERROR;
    case 'W': return APP_LOG_WARN;
    case 'I': return APP_LOG_INFO;
    default:  return APP_LOG_DEBUG;     /* D, V, and anything unrecognised */
    }
}

/* Remove ANSI colour sequences and the line ending, in place. */
static void strip(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '\033') {
            while (*r && *r != 'm') {
                r++;
            }
            if (*r == '\0') {
                break;
            }
            continue;
        }
        if (*r != '\r' && *r != '\n') {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* "I (1234) tag: message" -> level, tag, message. Anything else is passed on whole. */
static void forward(char *raw)
{
    strip(raw);
    if (raw[0] == '\0') {
        return;
    }

    app_evt_t evt = { .type = APP_EVT_LOG, .lvl = '?', .msg = raw };
    char *close = strstr(raw, ") ");
    if (raw[1] == ' ' && raw[2] == '(' && close != NULL) {
        char *tag = close + 2;
        char *sep = strstr(tag, ": ");
        if (sep != NULL) {
            *sep = '\0';
            evt.lvl = raw[0];
            evt.log_tag = tag;
            evt.msg = sep + 2;
        }
    }
    if (level_of(evt.lvl) > (app_log_level_t)atomic_load(&s_level)) {
        return;
    }

    const size_t n = proto_format(&evt, s_line, sizeof(s_line));
    if (n > 0) {
        usb_cdc_send(s_line, n, 0);
    }
}

static int log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int written = s_console(fmt, args);

    /* Sending can itself log; s_busy makes that inner line console-only instead of recursing. */
    if (atomic_load(&s_level) != APP_LOG_OFF && !xPortInIsrContext()
            && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING
            && !atomic_flag_test_and_set(&s_busy)) {
        vsnprintf(s_raw, sizeof(s_raw), fmt, copy);
        forward(s_raw);
        atomic_flag_clear(&s_busy);
    }
    va_end(copy);
    return written;
}

void log_forward_init(void)
{
    s_console = esp_log_set_vprintf(log_vprintf);
}

void log_forward_set_level(app_log_level_t level)
{
    atomic_store(&s_level, level);
    esp_log_level_set("*", level == APP_LOG_DEBUG ? ESP_LOG_DEBUG : ESP_LOG_INFO);
    /* "*" resets every tag, including the one the PN532 driver silenced: the I2C driver
     * reports each read the chip refuses, which for this chip is several a second and routine. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    /* The HTTP client's debug level prints every header it sends, the token among them. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_INFO);
}
