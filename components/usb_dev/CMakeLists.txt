set(srcs "usb_cdc.c")

# The keyboard and the composite descriptors exist only when TinyUSB has a HID interface.
if(CONFIG_TINYUSB_HID_COUNT)
    list(APPEND srcs "usb_hid_kbd.c" "usb_descriptors.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ".")
