idf_component_register(SRCS "widf_mngr_main.c"
                             "widf_mngr_handlers.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_http_server spi_flash
                                  esp_driver_tsens esp_driver_gpio esp_app_format app_update
                                  esp_timer esp_partition
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "Top_portal.html"
                                   "Bottom_portal.html")
