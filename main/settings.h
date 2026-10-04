/* The few settings the device keeps across power cycles, in NVS. */
#pragma once

#include <stdbool.h>

/* Open the store. Erases and starts again if what is in the partition cannot be read. */
void settings_init(void);

/* Whether a tap types its text record when no session has said otherwise. */
bool settings_hid_default(void);
void settings_set_hid_default(bool enabled);
