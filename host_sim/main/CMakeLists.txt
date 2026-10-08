idf_component_register(SRCS "sim_main.c" "sim_net.c" "sim_ota.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_core proto ota_stream net_sync sim_ntag)
