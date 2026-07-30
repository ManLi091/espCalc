idf_component_register(SRCS "main.c"
                            "camera.c"
                            "network.c"
                            "display.c"
                            "u8g2_hal.c"
                            "test_server.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp32-camera esp_http_client
                                  esp-tls json mbedtls esp_psram esp_hw_support u8g2 heap
                                  esp_http_server
                    INCLUDE_DIRS "."
                    REQUIRES driver)
