#ifndef U8G2_HAL_H
#define U8G2_HAL_H

#include "u8g2.h"

// Sets up the SPI bus/device and CS/DC/RESET GPIOs (pins from config.h),
// then wires up u8g2 for the GMG12864-06D (ST7565R, EA DOGM128-compatible
// controller config) over hardware SPI. Call once at boot.
void u8g2_hal_init(u8g2_t *u8g2);

#endif
