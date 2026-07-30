// network.c -> WiFi connect (preconfigured credentials) + OpenAI vision
// request. No NVS credential storage, no background tasks — this
// firmware does one thing at a time (wake -> connect -> capture -> ask
// -> display -> sleep), so everything here is called synchronously from
// main.c.

#include "network.h"
#include "config.h"
#include "secret.h"
#include "camera.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "NET";

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect_blocking(uint32_t timeout_ms) {

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connect failed/timed out");
    return ESP_FAIL;
}

void wifi_get_ip_str(char *out, size_t out_len) {
    esp_netif_ip_info_t ip_info;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(out, out_len, "?.?.?.?");
    }
}

// ---------------------------------------------------------------------
// OpenAI vision request
// ---------------------------------------------------------------------
//
// NOTE: large allocations here (base64 image string, JSON request body)
// rely on CONFIG_SPIRAM_USE_MALLOC=y (sdkconfig.defaults) to route big
// malloc()/cJSON allocations into PSRAM automatically.

static char s_answer[MAX_ANSWER_CHARS + 1];

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_buf_t;

static esp_err_t vision_http_event_handler(esp_http_client_event_t *evt) {
    http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        if (rb->len + evt->data_len + 1 > rb->cap) {
            size_t new_cap = rb->cap == 0 ? 4096 : rb->cap * 2;
            while (new_cap < rb->len + evt->data_len + 1) new_cap *= 2;
            char *grown = (char *)realloc(rb->buf, new_cap);
            if (!grown) {
                ESP_LOGE(TAG, "realloc failed growing response buffer");
                return ESP_FAIL;
            }
            rb->buf = grown;
            rb->cap = new_cap;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->buf[rb->len] = '\0';
    }
    return ESP_OK;
}

static void set_error(const char *msg) {
    memset(s_answer, 0, sizeof(s_answer));
    strncpy(s_answer, msg, MAX_ANSWER_CHARS);
}

// ---------------------------------------------------------------------
// TEST MODE: the actual OpenAI request is disabled below (wrapped in
// #if 0) so you can test capture + display rendering without spending
// API credits or needing working credentials. main.c no longer calls
// this function in test mode — see test_server.c instead, which serves
// the captured photo over HTTP and lets you paste AI-style test text
// straight into display_set_text().
//
// To re-enable live OpenAI calls: change the `#if 0` below to `#if 1`,
// and switch main.c's capture_and_display() back to calling this
// function instead of test_server's capture-and-serve flow.
// ---------------------------------------------------------------------
#if 0
char *request_vision_answer(void) {

    memset(s_answer, 0, sizeof(s_answer));

    if (!camera_is_ready()) {
        set_error("ERROR: CAMERA NOT READY");
        return s_answer;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        set_error("ERROR: CAPTURE FAILED");
        return s_answer;
    }

    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, fb->buf, fb->len);
    char *b64_buf = (char *)malloc(b64_len + 1);
    if (!b64_buf) {
        esp_camera_fb_return(fb);
        set_error("ERROR: OUT OF MEMORY");
        return s_answer;
    }
    size_t b64_written = 0;
    mbedtls_base64_encode((unsigned char *)b64_buf, b64_len + 1, &b64_written, fb->buf, fb->len);
    b64_buf[b64_written] = '\0';
    esp_camera_fb_return(fb);

    const char *prefix = "data:image/jpeg;base64,";
    size_t data_uri_len = strlen(prefix) + b64_written + 1;
    char *data_uri = (char *)malloc(data_uri_len);
    if (!data_uri) {
        free(b64_buf);
        set_error("ERROR: OUT OF MEMORY");
        return s_answer;
    }
    snprintf(data_uri, data_uri_len, "%s%s", prefix, b64_buf);
    free(b64_buf);

    // Build the JSON request body: system prompt (formatting rules) +
    // user prompt + image.

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(data_uri);
        set_error("ERROR: OUT OF MEMORY");
        return s_answer;
    }
    cJSON_AddStringToObject(root, "model", OPENAI_VISION_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 800);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *system_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", OPENAI_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, system_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(user_msg, "content");

    cJSON *text_block = cJSON_CreateObject();
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", OPENAI_VISION_USER_PROMPT);
    cJSON_AddItemToArray(content, text_block);

    cJSON *image_block = cJSON_CreateObject();
    cJSON_AddStringToObject(image_block, "type", "image_url");
    cJSON *image_url = cJSON_AddObjectToObject(image_block, "image_url");
    cJSON_AddStringToObject(image_url, "url", data_uri);
    cJSON_AddItemToArray(content, image_block);

    cJSON_AddItemToArray(messages, user_msg);

    free(data_uri);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        set_error("ERROR: JSON BUILD FAILED");
        return s_answer;
    }

    http_resp_buf_t resp = {0};

    char auth_header[64 + sizeof(OPENAI_API_KEY)];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);

    esp_http_client_config_t config = {
        .url = "https://api.openai.com/v1/chat/completions",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = vision_http_event_handler,
        .user_data = &resp,
    };

    esp_err_t perform_err = ESP_FAIL;
    int status = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&resp, 0, sizeof(resp));
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            perform_err = ESP_FAIL;
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", auth_header);
        esp_http_client_set_post_field(client, body, strlen(body));

        perform_err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (perform_err == ESP_OK) break;

        ESP_LOGW(TAG, "HTTP attempt %d failed: %s — %s", attempt + 1,
                 esp_err_to_name(perform_err), attempt == 0 ? "retrying once" : "giving up");
        if (resp.buf) { free(resp.buf); resp.buf = NULL; resp.len = 0; resp.cap = 0; }
    }
    free(body);

    if (perform_err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(perform_err));
        if (resp.buf) free(resp.buf);
        set_error("ERROR: HTTP REQUEST FAILED");
        return s_answer;
    }

    ESP_LOGI(TAG, "HTTP status %d, %u bytes", status, (unsigned)resp.len);

    if (!resp.buf) {
        set_error("ERROR: EMPTY RESPONSE");
        return s_answer;
    }

    cJSON *resp_json = cJSON_Parse(resp.buf);
    free(resp.buf);

    if (!resp_json) {
        set_error("ERROR: BAD JSON RESPONSE");
        return s_answer;
    }

    cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
    cJSON *first_choice = (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = first_choice ? cJSON_GetObjectItem(first_choice, "message") : NULL;
    cJSON *content_str = message ? cJSON_GetObjectItem(message, "content") : NULL;

    if (content_str && cJSON_IsString(content_str)) {
        strncpy(s_answer, content_str->valuestring, MAX_ANSWER_CHARS);
    } else {
        cJSON *error_obj = cJSON_GetObjectItem(resp_json, "error");
        cJSON *error_msg = error_obj ? cJSON_GetObjectItem(error_obj, "message") : NULL;
        if (error_msg && cJSON_IsString(error_msg)) {
            strncpy(s_answer, error_msg->valuestring, MAX_ANSWER_CHARS);
        } else {
            set_error("ERROR: NO CONTENT IN RESPONSE");
        }
    }

    cJSON_Delete(resp_json);
    return s_answer;
}
#endif // #if 0 — end of disabled OpenAI request code
