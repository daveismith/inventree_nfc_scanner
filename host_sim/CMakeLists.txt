# The scanner as a host program: the real state machine and protocol over a simulated tag,
# on a pseudo-terminal. See main/sim_main.c.
#
#   idf.py --preview set-target linux && idf.py build && ./build/host_sim.elf
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS
     "../components/ndef"
     "../components/ntag21x"
     "../components/app_core"
     "../components/proto"
     "../host_test/components/sim_ntag")
project(host_sim)
