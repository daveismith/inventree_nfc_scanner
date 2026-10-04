# The frame codec is pure C and builds for the host tests; the driver needs the I2C master.
set(srcs "pn532_frame.c")
set(priv_requires "")

if(NOT ${IDF_TARGET} STREQUAL "linux")
    list(APPEND srcs "pn532.c")
    list(APPEND priv_requires esp_driver_i2c esp_driver_gpio esp_timer)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES ntag21x
                       PRIV_REQUIRES ${priv_requires})
