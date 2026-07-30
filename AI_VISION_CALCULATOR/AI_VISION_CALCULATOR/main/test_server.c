// test_server.c -> local HTTP server used only for testing. Serves the
// last captured photo and a page where you can paste AI-style response
// text and send it straight to display_set_text(), so the whole
// markup/rendering pipeline can be exercised without calling OpenAI.
//
// Runs on the httpd task, which is a different task from the one that
// calls display_set_text() on button presses (main.c) — both paths are
// mutex-protected in display.c (see s_display_mutex there) since they
// can genuinely race now that there are two callers.

#include "test_server.h"
#include "display.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TEST_SRV";

static uint8_t *s_photo_buf = NULL;
static size_t s_photo_len = 0;
static size_t s_photo_cap = 0;
static SemaphoreHandle_t s_photo_mutex;

static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>AI Vision Calculator - Test Mode</title></head><body>"
"<h3>AI Vision Calculator - Test Mode</h3>"
"<p>OpenAI calls are disabled (see network.c). Latest photo below; "
"paste AI-style response text using the ^{}, _{}, \\f{num}{den}, "
"\\sqrt{} markup and send it straight to the display.</p>"
"<img src='/photo.jpg' style='max-width:400px;display:block;margin-bottom:10px' "
"onerror=\"this.style.display='none'\">"
"<textarea id='t' rows='10' cols='42' "
"placeholder='e.g. Area = \\f{b*h}{2}&#10;x^{2} + 3x = 0&#10;\\sqrt{x+1}'></textarea><br>"
"<button onclick='go()'>Send to display</button> "
"<span id='status'></span>"
"<script>"
"function go(){"
"document.getElementById('status').innerText=' sending...';"
"fetch('/submit',{method:'POST',body:document.getElementById('t').value})"
".then(function(r){document.getElementById('status').innerText="
"r.ok?' sent!':' failed';})"
".catch(function(){document.getElementById('status').innerText=' failed';});"
"}"
"</script></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t photo_get_handler(httpd_req_t *req) {
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);
    if (!s_photo_buf || s_photo_len == 0) {
        xSemaphoreGive(s_photo_mutex);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t err = httpd_resp_send(req, (const char *)s_photo_buf, s_photo_len);
    xSemaphoreGive(s_photo_mutex);
    return err;
}

static esp_err_t submit_post_handler(httpd_req_t *req) {

    size_t total = req->content_len;
    if (total == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    if (total > MAX_ANSWER_CHARS) total = MAX_ANSWER_CHARS; // truncate rather than reject

    char *buf = (char *)heap_caps_malloc(MAX_ANSWER_CHARS + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "Received %u bytes of test text", (unsigned)received);
    display_set_text(buf);
    free(buf);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

void test_server_start(void) {
    if (!s_photo_mutex) {
        s_photo_mutex = xSemaphoreCreateMutex();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; // JPEG serving + text handling needs headroom beyond the httpd default

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start test server");
        return;
    }

    httpd_uri_t root_uri   = { .uri = "/",          .method = HTTP_GET,  .handler = root_get_handler };
    httpd_uri_t photo_uri  = { .uri = "/photo.jpg", .method = HTTP_GET,  .handler = photo_get_handler };
    httpd_uri_t submit_uri = { .uri = "/submit",    .method = HTTP_POST, .handler = submit_post_handler };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &photo_uri);
    httpd_register_uri_handler(server, &submit_uri);

    ESP_LOGI(TAG, "Test server started");
}

void test_server_set_photo(const uint8_t *jpg_buf, size_t len) {
    if (!s_photo_mutex) {
        s_photo_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);
    if (len > s_photo_cap) {
        uint8_t *grown = (uint8_t *)heap_caps_realloc(s_photo_buf, len, MALLOC_CAP_SPIRAM);
        if (!grown) {
            ESP_LOGE(TAG, "Failed to grow photo buffer to %u bytes", (unsigned)len);
            xSemaphoreGive(s_photo_mutex);
            return;
        }
        s_photo_buf = grown;
        s_photo_cap = len;
    }
    memcpy(s_photo_buf, jpg_buf, len);
    s_photo_len = len;
    xSemaphoreGive(s_photo_mutex);
}
