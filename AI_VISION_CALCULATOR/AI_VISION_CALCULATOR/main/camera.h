#ifndef CAMERA_H
#define CAMERA_H

#include "esp_err.h"
#include <stdbool.h>

// Initializes the OV5640 (pins from board_config.h/camera_pins.h) and
// applies exposure/gain/white-balance tuning. Call once after PSRAM is up.
esp_err_t camera_init(void);

bool camera_is_ready(void);

#endif
