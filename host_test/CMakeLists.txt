# Host-side unit tests for the pure-C components, built for ESP-IDF's `linux` target:
#
#   idf.py --preview set-target linux && idf.py build && ./build/host_test.elf
#
# Components are named one by one rather than adding ../components as a directory, so that the
# component manager never sees the ones that only make sense on the chip (USB, LED, buzzer).
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS
     "../components/ndef"
     "../components/ntag21x"
     "../components/pn532"
     "../components/app_core"
     "../components/proto"
     "../components/net_sync"
     "../components/wifi_sta"
     "components/sim_ntag")
project(host_test)
