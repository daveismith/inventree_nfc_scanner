#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

/* Read once at boot: why the chip reset, and whether the last run left a core dump. */
void sysinfo_init(void);

/* What `info` reports about the system. The strings live as long as the firmware runs. */
void sysinfo_get(app_sysinfo_t *out);

/* The reader's firmware version, once it has answered; `ok` false when it has stopped. */
void sysinfo_set_pn532(bool ok, uint8_t ic, uint8_t ver, uint8_t rev);

void sysinfo_set_buzzer(bool fitted);
