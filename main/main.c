/*
 * InvenTree NFC desk reader/programmer.
 *
 * A USB device that an InvenTree page drives over WebSerial with newline-delimited JSON
 * (components/proto), to write and read the NFC tags on storage bins.
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "app_download_mode.h"
#include "app_task.h"
#include "dev_recovery.h"
#include "feedback.h"
#include "log_forward.h"
#include "pn532.h"
#include "settings.h"
#include "sysinfo.h"
#include "usb_dev.h"

#include "ota.h"
#if CONFIG_APP_NET_ENABLE
#include "net_link.h"
#endif

/* A bool Kconfig option that is off is not defined at all. */
#ifdef CONFIG_APP_LED_ORDER_GRB
#define LED_ORDER_GRB true
#else
#define LED_ORDER_GRB false
#endif

static const char *TAG = "main";

static void on_line(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    app_task_line(APP_LINK_USB, line, len);
}

static void on_line_too_long(void *ctx)
{
    (void)ctx;
    app_task_line_too_long(APP_LINK_USB);
}

static void on_link(void *ctx, bool up)
{
    (void)ctx;
    app_task_link(APP_LINK_USB, up);
}

static void usb_send(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    usb_cdc_send(line, len, 100);
}

static void on_mount(void *ctx, bool mounted)
{
    (void)ctx;
    if (mounted) {
        dev_recovery_usb_mounted();
        ota_note_usb_host();
    }
}

#if CONFIG_APP_USB_TOUCH_1200
static void on_touch_1200(void *ctx)
{
    (void)ctx;
    /* Straight to the reboot task, past the protocol: this has to work when that is stuck. */
    app_download_mode_request();
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "InvenTree NFC scanner %s", esp_app_get_description()->version);

    /*
     * The order here is what makes a bad build recoverable without touching the board. The
     * way into download mode, the crash count and USB come first and do not change from one
     * build to the next; everything that does change comes after dev_recovery_usb_started(),
     * so however badly it fails, the guards are already in place to catch it.
     */
    app_download_mode_init();
    dev_recovery_init();
    app_task_init();
    app_task_add_link(APP_LINK_USB, false, usb_send, NULL);

    const usb_dev_config_t usb = {
        .on_line = on_line,
        .on_line_too_long = on_line_too_long,
        .on_link = on_link,
        .on_mount = on_mount,
#if CONFIG_APP_USB_TOUCH_1200
        .on_touch_1200 = on_touch_1200,    /* development builds only: see Kconfig */
#endif
    };
    ESP_ERROR_CHECK(usb_dev_init(&usb));
    if (dev_recovery_stay_off_usb()) {
        ESP_LOGW(TAG, "staying off the bus for this boot, as asked");
        usb_dev_disconnect();
    }
    if (dev_recovery_usb_started()) {
        /* On the way to download mode: start nothing else. */
        vTaskDelay(portMAX_DELAY);
    }

    sysinfo_init();
    log_forward_init();
    settings_init();
    ota_init();
#if CONFIG_APP_NET_ENABLE
    net_link_init();
#endif

    const feedback_config_t fb = {
        .led_gpio = CONFIG_APP_LED_GPIO,
        .led_grb = LED_ORDER_GRB,
        .buzzer_gpio = CONFIG_APP_BUZZER_GPIO,
        .buzzer_gpio_b = CONFIG_APP_BUZZER_GPIO_B,
        .buzzer_hz = CONFIG_APP_BUZZER_FREQ_HZ,
    };
    if (feedback_init(&fb) != ESP_OK) {
        ESP_LOGE(TAG, "no LED or buzzer");
    }
    sysinfo_set_buzzer(feedback_buzzer_fitted());

#if CONFIG_APP_NFC_ENABLE
    const pn532_config_t nfc = {
        .i2c_port = 0,
        .sda_gpio = CONFIG_APP_NFC_SDA_GPIO,
        .scl_gpio = CONFIG_APP_NFC_SCL_GPIO,
        .freq_hz = CONFIG_APP_NFC_I2C_FREQ_HZ,
        .irq_gpio = CONFIG_APP_NFC_IRQ_GPIO,
        .rst_gpio = CONFIG_APP_NFC_RST_GPIO,
    };
    /* Not fatal: with no bus the reader simply never comes up, and `info` says so. */
    if (pn532_init(&nfc) != ESP_OK) {
        ESP_LOGE(TAG, "cannot set up the I2C bus for the PN532");
    }
#endif

    app_task_start();
}
