idf_component_register(SRCS "sim_main.c" "sim_net.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_core proto net_sync sim_ntag)
