#pragma once

#include <stdint.h>

#include "tinyusb.h"

extern const uint8_t usb_hid_report_descriptor[];
extern const uint16_t usb_hid_report_descriptor_len;

/* Point a TinyUSB configuration at the composite CDC + HID descriptors. */
void usb_descriptors_fill(tinyusb_desc_config_t *descriptor);
