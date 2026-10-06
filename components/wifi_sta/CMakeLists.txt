# The Wi-Fi station. wifi_policy.c decides what to try next and is plain C (host tests);
# wifi_sta.c is the part that talks to esp_wifi, built only on the chip.
if(${IDF_TARGET} STREQUAL "linux")
    idf_component_register(SRCS "wifi_policy.c" INCLUDE_DIRS "include")
else()
    idf_component_register(SRCS "wifi_policy.c" "wifi_sta.c"
                           INCLUDE_DIRS "include"
                           REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash)
endif()
