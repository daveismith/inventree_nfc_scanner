/*
 * Reboot into the ROM download mode so the board can be flashed without being touched.
 *
 * The ESP32-S3 has one internal USB PHY, on GPIO19/20, and TinyUSB claims it for USB-OTG.
 * That leaves the USB-Serial-JTAG controller unreachable, so esptool cannot drive EN and
 * GPIO0 in hardware the way it does on a stock board -- hence the BOOT button. The way out
 * is to hand the PHY mux back to the ROM, ask the ROM for download mode, and restart.
 *
 * This is r2_domeplayer's main/app_download_mode.c, which was worked out on hardware, with
 * the console command and the DFU target removed. The sequence and the context it runs in
 * are deliberately unchanged; each has a failure behind it that only a power cycle cleared.
 * The register sequence mirrors esp_usb_console_before_restart() in
 * components/esp_usb_cdc_rom_console/usb_console.c, IDF's own (private) version of this.
 */

#include "app_download_mode.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/rtc_cntl_reg.h"

#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp32s3/rom/usb/usb_persist.h"

#include "usb_dev.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "app_download_mode uses ESP32-S3 ROM entry points and RTC_CNTL bit positions"
#endif

/* Long enough for the reply to the `bootloader` command to leave over CDC first. */
#define REPLY_GRACE_MS  100

static const char *TAG = "download";

static TaskHandle_t s_reboot_task;
static bool s_armed;

/* Runs from esp_restart(), before the scheduler is suspended but after everything else has
 * had its say. This is where IDF puts the same sequence, and it is the last point at which
 * the TinyUSB task is no longer competing for the USB core. Guarded on s_armed so that an
 * ordinary restart goes through untouched. */
static void app_download_mode_before_restart(void)
{
    if (!s_armed) {
        return;
    }

    /* The ROM keeps this word in the always-on domain, so it holds whatever was last written
     * to it -- and if USBDC_PERSIST_ENA (bit 31) happens to be set, the ROM skips USB
     * initialization entirely, on the assumption that the host still has its descriptors.
     * Download mode is entered, but nothing ever appears on the bus. */
    chip_usb_set_persist_flags(0);

    /* Hand PHY-mux selection back to hardware/eFuse control. TinyUSB set both of these to
     * route the internal PHY to USB-OTG, and they live in the RTC domain, so they survive the
     * CPU-only reset esp_restart() performs on this target -- usb_del_phy() does not put them
     * back. With USB_PHY_SEL unburned, hardware control means the internal PHY goes to
     * USB-Serial-JTAG, which is what the ROM expects. */
    REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);

    /* Sticky: nothing on the chip clears this. esptool's ESP32-S3 hard reset clears it
     * before resetting, which is what gets us back into the app after a flash. */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}

/*
 * Exists to give esp_restart() and the shutdown handlers a task and a stack of their own.
 *
 * The context matters. Deferring the restart to an esp_timer callback came back up in the app
 * with FORCE_DOWNLOAD_BOOT unset; restarting from a task sized for waiting on a socket left
 * the ROM not answering esptool at all. A dedicated 4096-byte task is what was verified, so
 * every trigger -- the `bootloader` command, the 1200 baud touch, the recovery guard -- only
 * notifies this one.
 */
static void reboot_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_LOGW(TAG, "rebooting into ROM download mode (USB-Serial-JTAG)");
    vTaskDelay(pdMS_TO_TICKS(REPLY_GRACE_MS));

    s_armed = true;
    usb_dev_disconnect();           /* tud_disconnect() */
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_restart();
}

void app_download_mode_init(void)
{
    ESP_ERROR_CHECK(esp_register_shutdown_handler(app_download_mode_before_restart));
    const BaseType_t made = xTaskCreate(reboot_task, "download_reboot", 4096, NULL, 5, &s_reboot_task);
    ESP_ERROR_CHECK(made == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void app_download_mode_request(void)
{
    if (s_reboot_task) {
        xTaskNotifyGive(s_reboot_task);
    }
}
