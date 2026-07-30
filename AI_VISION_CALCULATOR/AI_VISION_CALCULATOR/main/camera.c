#include "camera.h"
#include "esp_camera.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "board_config.h"

static const char *TAG = "CAMERA";
static bool s_camera_ready = false;

// Exposure/gain/white-balance tuning — see the chat history for the full
// reasoning; short version: caps AGC gain to avoid low-light noise/color
// cast, enables the correction registers the sensor supports.
static void tune_camera_sensor(sensor_t *s) {
  bool isOV5640 = (s->id.PID == OV5640_PID);

  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);

  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, 0);
  s->set_aec_value(s, 300);

  s->set_gain_ctrl(s, 1);
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, GAINCEILING_4X);

  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  s->set_hmirror(s, 0);
  s->set_colorbar(s, 0);

  if (!isOV5640) {
    s->set_sharpness(s, 0);
    s->set_denoise(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 1);
  }
}

esp_err_t camera_init(void) {
  camera_config_t config = {0};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;   // capture stills at full res for best vision-API accuracy
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  bool has_psram = esp_psram_is_initialized();
  if (has_psram) {
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    ESP_LOGW(TAG, "PSRAM not initialized — falling back to SVGA/DRAM");
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
    s_camera_ready = false;
    return err;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    ESP_LOGE(TAG, "esp_camera_sensor_get returned NULL");
    s_camera_ready = false;
    return ESP_FAIL;
  }

  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  tune_camera_sensor(s);

  s_camera_ready = true;
  ESP_LOGI(TAG, "Camera ready (PID 0x%02x)", s->id.PID);
  return ESP_OK;
}

bool camera_is_ready(void) {
  return s_camera_ready;
}
