idf_component_register(SRCS "sim_main.c" "sim_net.c" "sim_ota.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_core proto ota_stream net_sync sim_ntag)

# The version the simulator reports unless SIM_FW says otherwise: the firmware's, from
# version.txt, as the device reports it.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../version.txt" sim_fw_version LIMIT_COUNT 1)
string(STRIP "${sim_fw_version}" sim_fw_version)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SIM_FW_DEFAULT="${sim_fw_version}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
