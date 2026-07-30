// AI Vision Calculator — simplified firmware
//
// main.c -> 2x2 button matrix (capture / scroll up / scroll down), one
// spare position unused. Capture button wakes from deep sleep by holding
// ROW_A low through sleep (RTC GPIO hold) and waking on COL_A going low.
// Connects WiFi, takes a photo, sends it to OpenAI vision, displays the
// (word-wrapped) answer. No activity for INACTIVITY_SLEEP_MS -> sleep.

#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "config.h"
#include "display.h"
#include "camera.h"
#include "network.h"
#include "test_server.h"
#include "esp_camera.h"

static const char *TAG = "MAIN";

typedef enum {
    BTN_NONE,
    BTN_CAPTURE,
    BTN_SCROLL_UP,
    BTN_SCROLL_DOWN
} button_id_t;

// Scans the 2x2 matrix once and returns whichever of the 3 wired
// positions is currently pressed (or BTN_NONE). Leaves both rows idle
// (high) when it returns.
static button_id_t matrix_scan(void) {

    gpio_set_level(BTN_ROW_A_GPIO, 0);
    gpio_set_level(BTN_ROW_B_GPIO, 1);
    esp_rom_delay_us(20); // let the row line settle before reading columns
    bool row_a_col_a = (gpio_get_level(BTN_COL_A_GPIO) == 0);
    bool row_a_col_b = (gpio_get_level(BTN_COL_B_GPIO) == 0);

    gpio_set_level(BTN_ROW_A_GPIO, 1);
    gpio_set_level(BTN_ROW_B_GPIO, 0);
    esp_rom_delay_us(20);
    bool row_b_col_a = (gpio_get_level(BTN_COL_A_GPIO) == 0);
    // row_b_col_b intersection is unused/spare — not read

    gpio_set_level(BTN_ROW_A_GPIO, 1);
    gpio_set_level(BTN_ROW_B_GPIO, 1); // both rows idle high between scans

    if (row_a_col_a) return BTN_CAPTURE;
    if (row_a_col_b) return BTN_SCROLL_UP;
    if (row_b_col_a) return BTN_SCROLL_DOWN;
    return BTN_NONE;
}

static void configure_matrix_for_active_scanning(void) {

    // Release anything left over from a previous deep-sleep hold before
    // reconfiguring these pins as regular digital GPIO.
    gpio_hold_dis(BTN_ROW_A_GPIO);
    rtc_gpio_hold_dis(BTN_ROW_A_GPIO);
    rtc_gpio_deinit(BTN_ROW_A_GPIO);
    rtc_gpio_deinit(BTN_COL_A_GPIO);

    uint64_t row_mask = (1ULL << BTN_ROW_A_GPIO) | (1ULL << BTN_ROW_B_GPIO);
    gpio_config_t row_conf = {
        .pin_bit_mask = row_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&row_conf);
    gpio_set_level(BTN_ROW_A_GPIO, 1);
    gpio_set_level(BTN_ROW_B_GPIO, 1);

    uint64_t col_mask = (1ULL << BTN_COL_A_GPIO) | (1ULL << BTN_COL_B_GPIO);
    gpio_config_t col_conf = {
        .pin_bit_mask = col_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&col_conf);
}

// TEST MODE: captures a photo, serves it at /photo.jpg via test_server,
// and shows the URL to visit — instead of calling OpenAI. Visit that
// URL to see the photo and paste test AI-style response text straight
// to the display. To go back to live OpenAI calls: swap the body of
// this function back to display_show_message("Thinking...") +
// request_vision_answer() + display_set_text(answer) (see the #if 0
// block in network.c, which still has that code intact).
static void capture_and_display(void) {
    display_set_text("Capturing photo...");

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        display_set_text("ERROR: capture failed");
        return;
    }
    test_server_set_photo(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    char ip[16] = "?";
    wifi_get_ip_str(ip, sizeof(ip));
    char msg[128];
    snprintf(msg, sizeof(msg),
             "Photo captured.\n\nOpen this in a browser:\nhttp://%s/\n\n"
             "(paste test AI text there to test the display)", ip);
    display_set_text(msg);
}

// Holds ROW_A low through deep sleep and arms COL_A as the EXT1 wake
// source, so pressing the capture button (ROW_A x COL_A) wakes the chip
// with no software running. Never returns.
static void enter_deep_sleep(void) {
    ESP_LOGI(TAG, "Going to deep sleep");
    display_show_message("Sleeping...");
    vTaskDelay(pdMS_TO_TICKS(300));
    display_clear();

    // Hold ROW_A, COL_A, and PE through deep sleep. PE especially matters:
    // if your power-latch circuit needs this pin actively driven high to
    // stay powered (rather than a one-shot latch), letting it float during
    // sleep could cut power to the whole board instead of actually
    // sleeping. Holding it is harmless even if your circuit turns out not
    // to need it.
    rtc_gpio_init(PE);
    rtc_gpio_set_direction(PE, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(PE, 1);
    rtc_gpio_hold_en(PE);

    // Take the row/col pins away from regular GPIO control and hand
    // ROW_A to the RTC domain so its output level survives deep sleep.
    rtc_gpio_init(BTN_ROW_A_GPIO);
    rtc_gpio_set_direction(BTN_ROW_A_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(BTN_ROW_A_GPIO, 0);
    rtc_gpio_hold_en(BTN_ROW_A_GPIO);

    // COL_A also needs to be in the RTC domain with its pull-up alive
    // during sleep, since it's the wake pin.
    rtc_gpio_init(BTN_COL_A_GPIO);
    rtc_gpio_set_direction(BTN_COL_A_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BTN_COL_A_GPIO);
    rtc_gpio_pulldown_dis(BTN_COL_A_GPIO);

    // Required for the per-pin holds above to actually persist through
    // deep sleep, and for ROW_B/COL_B (left on normal GPIO) not to matter.
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_ext1_wakeup(1ULL << BTN_COL_A_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start(); // does not return
}

void app_main(void) {

    // Release the global deep-sleep GPIO hold first (harmless no-op on a
    // cold boot where nothing was ever held) — must happen before we try
    // to reconfigure any previously-held pin, including PE below.
    gpio_deep_sleep_hold_dis();
    rtc_gpio_hold_dis(PE);
    rtc_gpio_deinit(PE);

    // Power latch — MUST be driven high early, or the board can lose
    // power right after boot depending on your power-switch circuit.
    // This was defined in config.h but never actually set anywhere in
    // the earlier version of this file — real bug, fixed here.
    gpio_config_t pe_conf = {
        .pin_bit_mask = (1ULL << PE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&pe_conf);
    gpio_set_level(PE, 1);

    nvs_flash_init(); // required internally by the WiFi driver for PHY calibration data

    // Reconfigure the button matrix pins for active scanning (releases
    // their own prior RTC hold internally).
    configure_matrix_for_active_scanning();

    display_init();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %d", cause);

    display_show_message("Connecting...");
    if (wifi_connect_blocking(WIFI_CONNECT_TIMEOUT_MS) != ESP_OK) {
        display_show_message("WiFi failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
        enter_deep_sleep();
    }

    if (camera_init() != ESP_OK) {
        display_show_message("Camera failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
        enter_deep_sleep();
    }

    test_server_start(); // TEST MODE: local web server instead of calling OpenAI

    capture_and_display();

    int64_t last_activity_us = esp_timer_get_time();
    int64_t last_press_ms = 0;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        int64_t now_ms = now_us / 1000;

        if ((now_ms - last_press_ms) > BUTTON_DEBOUNCE_MS) {
            button_id_t pressed = matrix_scan();
            if (pressed != BTN_NONE) {
                last_press_ms = now_ms;
                last_activity_us = now_us;
                switch (pressed) {
                    case BTN_SCROLL_UP:   display_scroll_up();   break;
                    case BTN_SCROLL_DOWN: display_scroll_down(); break;
                    case BTN_CAPTURE:     capture_and_display(); break; // retake, same flow
                    default: break;
                }
            }
        }

        if ((now_us - last_activity_us) / 1000 > INACTIVITY_SLEEP_MS) {
            enter_deep_sleep();
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}
