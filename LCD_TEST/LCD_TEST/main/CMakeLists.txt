idf_component_register(SRCS "main.c"
                            "display.c"
                            "u8g2_hal.c"
                    PRIV_REQUIRES u8g2 heap
                    INCLUDE_DIRS "."
                    REQUIRES driver)
