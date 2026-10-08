cmake_minimum_required(VERSION 3.22)

# The firmware version, reported by `info` as "fw", comes from version.txt, which IDF reads
# by itself. A release tag must match it (tools/make_release.py checks).

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(inventree_nfc_scanner)
