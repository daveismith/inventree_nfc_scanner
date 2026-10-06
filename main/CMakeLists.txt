set(srcs "main.c"
         "app_task.c"
         # The way into ROM download mode. Ported from r2_domeplayer and kept unchanged
         # between builds; see the comments in the file.
         "app_download_mode.c"
         "dev_recovery.c"
         "log_forward.c"
         "settings.c"
         "sysinfo.c")

# The network link and the updater are in the build only when asked (menuconfig: InvenTree
# NFC scanner -> Network). What they need is always listed: the build system settles a
# component's requirements before the configuration is read, so they cannot be conditional.
# Without the two sources nothing refers to Wi-Fi, TLS or the HTTP client, and the linker
# leaves them out.
if(CONFIG_APP_NET_ENABLE)
    list(APPEND srcs "net_link.c" "ota.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       # MINIMAL_BUILD: only what is named here, and what it needs, is built.
                       REQUIRES app_core proto usb_dev pn532 feedback net_sync wifi_sta
                                esp_app_format esp_timer espcoredump nvs_flash
                                esp_wifi esp_netif esp_event esp_http_client esp_https_ota
                                app_update mbedtls esp-tls)
