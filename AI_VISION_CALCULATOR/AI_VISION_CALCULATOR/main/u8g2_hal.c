// u8g2_hal.c -> ESP-IDF hardware-SPI HAL for u8g2, following the standard
// porting pattern (u8x8_byte_* + u8x8_gpio_and_delay callbacks) documented
// at https://github.com/olikraus/u8g2/wiki/Porting-to-new-MCU-platform

#include "u8g2_hal.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "U8G2_HAL";
static spi_device_handle_t s_spi;

static uint8_t u8x8_byte_hw_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_BYTE_SEND: {
            spi_transaction_t t = {0};
            t.length = 8 * arg_int; // bits
            t.tx_buffer = arg_ptr;
            esp_err_t err = spi_device_transmit(s_spi, &t);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "spi_device_transmit failed: 0x%x", err);
            }
            break;
        }
        case U8X8_MSG_BYTE_INIT: {
            spi_bus_config_t bus_cfg = {
                .mosi_io_num = LCD_SI_GPIO,
                .miso_io_num = -1,
                .sclk_io_num = LCD_SCL_GPIO,
                .quadwp_io_num = -1,
                .quadhd_io_num = -1,
                .max_transfer_sz = 4096,
            };
            ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

            spi_device_interface_config_t dev_cfg = {
                .clock_speed_hz = 4 * 1000 * 1000, // 4 MHz — conservative for a jumper-wired display; raise if signal integrity allows
                .mode = 0,
                .spics_io_num = -1, // CS handled manually below (u8g2 toggles it via START/END_TRANSFER)
                .queue_size = 1,
            };
            ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi));
            break;
        }
        case U8X8_MSG_BYTE_SET_DC:
            gpio_set_level(LCD_DC_GPIO, arg_int);
            break;
        case U8X8_MSG_BYTE_START_TRANSFER:
            gpio_set_level(LCD_CS_GPIO, 0);
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            gpio_set_level(LCD_CS_GPIO, 1);
            break;
        default:
            return 0;
    }
    return 1;
}

static uint8_t u8x8_gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT: {
            uint64_t pin_mask = (1ULL << LCD_CS_GPIO) | (1ULL << LCD_DC_GPIO) | (1ULL << LCD_RES_GPIO);
            gpio_config_t io_conf = {
                .pin_bit_mask = pin_mask,
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            gpio_config(&io_conf);
            gpio_set_level(LCD_CS_GPIO, 1);
            break;
        }
        case U8X8_MSG_DELAY_MILLI:
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;
        case U8X8_MSG_DELAY_10MICRO:
            esp_rom_delay_us(arg_int * 10);
            break;
        case U8X8_MSG_DELAY_100NANO:
            esp_rom_delay_us(1); // close enough — ESP-IDF has no sub-microsecond delay
            break;
        case U8X8_MSG_GPIO_CS:
            gpio_set_level(LCD_CS_GPIO, arg_int);
            break;
        case U8X8_MSG_GPIO_DC:
            gpio_set_level(LCD_DC_GPIO, arg_int);
            break;
        case U8X8_MSG_GPIO_RESET:
            gpio_set_level(LCD_RES_GPIO, arg_int);
            break;
        default:
            return 0;
    }
    return 1;
}

void u8g2_hal_init(u8g2_t *u8g2) {
    // GMG12864-06D uses an ST7565R controller and is confirmed to work
    // with u8g2's "EA DOGM128" setup (same controller family/init sequence).
    // Full-framebuffer variant (_f) — 1024 bytes for 128x64x1bpp, easily
    // fits internal RAM. Required because display.c draws everything once
    // and calls u8g2_SendBuffer() a single time; the _1 (single-page)
    // variant only keeps 1/8th of the screen in RAM at a time and needs a
    // u8g2_FirstPage()/u8g2_NextPage() redraw loop instead — using it here
    // would only ever render the top strip of the screen.
    u8g2_Setup_st7565_ea_dogm128_f(u8g2, U8G2_R0, u8x8_byte_hw_spi, u8x8_gpio_and_delay);
    u8g2_InitDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0);
    u8g2_ClearBuffer(u8g2);
    u8g2_SendBuffer(u8g2);
}
