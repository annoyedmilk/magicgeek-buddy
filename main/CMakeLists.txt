# GLOB the species so adding or removing a buddy is a file drop in
# buddies/ with no build-system change required.
file(GLOB BUDDY_SPECIES_SRCS "buddies/*.c")

idf_component_register(SRCS "main.c"
                            "display.c"
                            "framebuffer.c"
                            "gfx.c"
                            "touch_button.c"
                            "storage.c"
                            "wifi_manager.c"
                            "ble_nus.c"
                            "bridge.c"
                            "stats.c"
                            "buddy.c"
                            ${BUDDY_SPECIES_SRCS}
                            "ui.c"
                            "ota_server.c"
                            "debug_log.c"
                            "net_trust.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_spi esp_driver_gpio
                             esp_driver_ledc esp_driver_touch_sens
                             nvs_flash spi_flash
                             espressif__cjson
                             esp_wifi esp_http_server esp_netif esp_event
                             esp_timer
                             bt app_update)
