#include "dev_recovery.h"

#include <stdint.h>
#include <stdlib.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_download_mode.h"

#if CONFIG_APP_DEV_RECOVERY

#define MAGIC 0x52435652u   /* "RCVR" */

static const char *TAG = "recovery";

/* Survives every reset short of losing power. Not initialised on purpose. */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t crashes_in_a_row;
    uint32_t stay_off_usb;
} s_rtc;

static volatile bool s_mounted;
static bool s_stay_off_usb;

static bool crashed(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT
           || reason == ESP_RST_WDT || reason == ESP_RST_CPU_LOCKUP;
}

void dev_recovery_init(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    if (s_rtc.magic != MAGIC || reason == ESP_RST_POWERON) {
        s_rtc.magic = MAGIC;
        s_rtc.crashes_in_a_row = 0;
        s_rtc.stay_off_usb = 0;
    }
    if (crashed(reason)) {
        s_rtc.crashes_in_a_row++;
    } else {
        s_rtc.crashes_in_a_row = 0;
    }
    s_stay_off_usb = s_rtc.stay_off_usb != 0;
    s_rtc.stay_off_usb = 0;             /* one boot only */
    ESP_LOGI(TAG, "reset reason %d, crashes in a row %u", (int)reason, (unsigned)s_rtc.crashes_in_a_row);
}

bool dev_recovery_stay_off_usb(void)
{
    return s_stay_off_usb;
}

/* esp_timer task: only signal. */
static void healthy_cb(void *arg)
{
    (void)arg;
    s_rtc.crashes_in_a_row = 0;
}

static void mount_timeout_cb(void *arg)
{
    (void)arg;
    if (!s_mounted) {
        ESP_LOGE(TAG, "no host enumerated the device in %d s: entering download mode",
                 CONFIG_APP_DEV_RECOVERY_MOUNT_TIMEOUT_S);
        app_download_mode_request();
    }
}

static void start_once(esp_timer_cb_t cb, const char *name, int seconds)
{
    const esp_timer_create_args_t args = { .callback = cb, .name = name };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)seconds * 1000000ULL);
    }
}

bool dev_recovery_usb_started(void)
{
    if (s_rtc.crashes_in_a_row >= CONFIG_APP_DEV_RECOVERY_BOOT_LOOPS) {
        ESP_LOGE(TAG, "%u crashes in a row: entering download mode", (unsigned)s_rtc.crashes_in_a_row);
        s_rtc.crashes_in_a_row = 0;
        app_download_mode_request();
        return true;
    }
    start_once(healthy_cb, "rcvr_healthy", CONFIG_APP_DEV_RECOVERY_HEALTHY_S);
    start_once(mount_timeout_cb, "rcvr_mount", CONFIG_APP_DEV_RECOVERY_MOUNT_TIMEOUT_S);
    return false;
}

void dev_recovery_usb_mounted(void)
{
    s_mounted = true;
}

bool dev_recovery_debug_available(void)
{
    return true;
}

void dev_recovery_debug(app_debug_action_t action)
{
    vTaskDelay(pdMS_TO_TICKS(100));     /* let the reply leave first */
    switch (action) {
    case APP_DEBUG_CRASH:
        ESP_LOGE(TAG, "debug: crashing on request");
        abort();
        break;
    case APP_DEBUG_HANG:
        ESP_LOGE(TAG, "debug: hanging on request");
        for (;;) {
            /* never yields: the task watchdog has to notice */
        }
        break;
    case APP_DEBUG_NOUSB:
        ESP_LOGE(TAG, "debug: restarting to stay off the bus");
        s_rtc.stay_off_usb = 1;
        esp_restart();
        break;
    }
}

#else /* !CONFIG_APP_DEV_RECOVERY */

void dev_recovery_init(void) { }
bool dev_recovery_stay_off_usb(void) { return false; }
bool dev_recovery_usb_started(void) { return false; }
void dev_recovery_usb_mounted(void) { }
bool dev_recovery_debug_available(void) { return false; }
void dev_recovery_debug(app_debug_action_t action) { (void)action; }

#endif
