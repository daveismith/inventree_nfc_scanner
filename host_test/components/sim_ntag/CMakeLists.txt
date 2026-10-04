idf_component_register(SRCS "sim_ntag.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ntag21x
                       PRIV_REQUIRES ndef)
