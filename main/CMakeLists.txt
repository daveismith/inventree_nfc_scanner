idf_component_register(SRCS "main.c"
                            "app_task.c"
                            # The way into ROM download mode. Ported from r2_domeplayer and kept
                            # unchanged between builds; see the comments in the file.
                            "app_download_mode.c"
                            "dev_recovery.c"
                            "log_forward.c"
                            "settings.c"
                            "sysinfo.c"
                       INCLUDE_DIRS "."
                       # MINIMAL_BUILD: only what is named here, and what it needs, is built.
                       REQUIRES app_core proto usb_dev pn532 feedback
                                esp_app_format esp_timer espcoredump nvs_flash)
