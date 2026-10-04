#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "sysinfo";

static const char *s_reset = "unknown";
static char s_crash[160];
static bool s_have_crash;
static bool s_pn532_ok;
static uint8_t s_pn532_ic, s_pn532_ver, s_pn532_rev;
static bool s_buzzer;

static const char *reset_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "external";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
    }
}

/* With no one watching the UART, the panic handler's output is lost. The core dump it leaves
 * in flash is not: this turns it into one line -- the task, where it died, and the call chain
 * as addresses to hand to addr2line with the matching ELF. */
static void read_crash_summary(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *summary = malloc(sizeof(*summary));
    if (summary == NULL) {
        return;
    }
    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        /* The first characters of the ELF hash say which build crashed; a dump stays in flash
         * until the next crash overwrites it, so it may predate the firmware now running. */
        int at = snprintf(s_crash, sizeof(s_crash), "task=%.16s pc=0x%08x elf=%.8s bt=",
                          summary->exc_task, (unsigned)summary->exc_pc, (const char *)summary->app_elf_sha256);
        for (uint32_t i = 0; i < summary->exc_bt_info.depth && i < 8 && at < (int)sizeof(s_crash) - 12; i++) {
            at += snprintf(s_crash + at, sizeof(s_crash) - (size_t)at, "%s0x%08x", i ? "," : "",
                           (unsigned)summary->exc_bt_info.bt[i]);
        }
        /* Say so when the dump is not from the firmware now running. */
        char running[9] = { 0 };
        esp_app_get_elf_sha256(running, sizeof(running));
        if (strncmp(running, (const char *)summary->app_elf_sha256, 8) != 0 && at < (int)sizeof(s_crash) - 20) {
            snprintf(s_crash + at, sizeof(s_crash) - (size_t)at, " (an earlier build)");
        }
        s_have_crash = true;
        ESP_LOGW(TAG, "core dump in flash: %s", s_crash);
    }
    free(summary);
#endif
}

void sysinfo_init(void)
{
    s_reset = reset_name(esp_reset_reason());
    read_crash_summary();
}

void sysinfo_get(app_sysinfo_t *out)
{
    out->fw = esp_app_get_description()->version;
    out->idf = esp_get_idf_version();
    out->pn532_ok = s_pn532_ok;
    out->pn532_ic = s_pn532_ic;
    out->pn532_ver = s_pn532_ver;
    out->pn532_rev = s_pn532_rev;
    out->buzzer = s_buzzer;
    out->reset = s_reset;
    out->crash = s_have_crash ? s_crash : NULL;
    out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void sysinfo_set_pn532(bool ok, uint8_t ic, uint8_t ver, uint8_t rev)
{
    s_pn532_ok = ok;
    s_pn532_ic = ic;
    s_pn532_ver = ver;
    s_pn532_rev = rev;
}

void sysinfo_set_buzzer(bool fitted)
{
    s_buzzer = fitted;
}
