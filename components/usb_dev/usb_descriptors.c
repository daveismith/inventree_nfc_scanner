/*
 * USB descriptors for the composite device: CDC-ACM for the protocol, plus a HID keyboard.
 *
 * esp_tinyusb builds descriptors itself for CDC alone, and that is what is used when the
 * keyboard is configured out. It has none for HID, so with the keyboard in, all three tables
 * are supplied here.
 *
 * Endpoints: the ESP32-S3's controller has five IN endpoints and EP0 is one of them. CDC
 * takes two (notification and data), HID one. That is also why logs cannot have a second CDC
 * interface of their own: it would need a sixth.
 */
#include "usb_descriptors.h"

#include <stdio.h>

#include "esp_mac.h"
#include "sdkconfig.h"
#include "tusb.h"

enum {
    ITF_CDC_COMM = 0,
    ITF_CDC_DATA,
    ITF_HID,
    ITF_COUNT,
};

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_HID,
    STR_COUNT,
};

#define EP_CDC_NOTIF    0x81
#define EP_CDC_OUT      0x02
#define EP_CDC_IN       0x82
#define EP_HID_IN       0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

/* Interface association descriptors are in use (CDC), which this class triple announces. */
static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = CONFIG_TINYUSB_DESC_CUSTOM_PID,
    .bcdDevice = CONFIG_TINYUSB_DESC_BCD_DEVICE,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

/* A plain boot keyboard: one report, no report ID. */
const uint8_t usb_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};
const uint16_t usb_hid_report_descriptor_len = sizeof(usb_hid_report_descriptor);

static const uint8_t s_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC_COMM, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD, sizeof(usb_hid_report_descriptor),
                       EP_HID_IN, 8, 10),
};

static char s_serial[13];

/* esp_tinyusb caps a string at 31 characters. */
static const char *s_strings[STR_COUNT] = {
    [STR_LANGID] = (const char[]){ 0x09, 0x04 },        /* English (US) */
    [STR_MANUFACTURER] = CONFIG_TINYUSB_DESC_MANUFACTURER_STRING,
    [STR_PRODUCT] = CONFIG_TINYUSB_DESC_PRODUCT_STRING,
    [STR_SERIAL] = s_serial,
    [STR_CDC] = CONFIG_TINYUSB_DESC_CDC_STRING,
    [STR_HID] = "InvenTree NFC Scanner keyboard",
};

void usb_descriptors_fill(tinyusb_desc_config_t *descriptor)
{
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    descriptor->device = &s_device;
    descriptor->full_speed_config = s_configuration;
    descriptor->string = s_strings;
    descriptor->string_count = STR_COUNT;
}
