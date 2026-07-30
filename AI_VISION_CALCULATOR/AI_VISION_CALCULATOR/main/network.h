#ifndef NETWORK_H
#define NETWORK_H

#include "esp_err.h"
#include <stddef.h>

// Connects to the WiFi network in secret.h. Blocks until connected or
// until timeout_ms elapses. Safe to call again after a deep sleep wake.
esp_err_t wifi_connect_blocking(uint32_t timeout_ms);

// Writes the STA IP as a dotted-decimal string (e.g. "192.168.1.42")
// into out. Used by the test-mode status message so you know what URL
// to open. Call only after wifi_connect_blocking() succeeds.
void wifi_get_ip_str(char *out, size_t out_len);

// TEST MODE: disabled (see the #if 0 block in network.c). Captures a
// frame and sends it + OPENAI_VISION_USER_PROMPT to OpenAI's vision
// endpoint, with OPENAI_SYSTEM_PROMPT as the system message. Not called
// anywhere right now — test_server.c's /submit handler feeds test text
// straight to display_set_text() instead. Re-enable by flipping the
// #if 0 to #if 1 in network.c and switching main.c back to calling this.
char *request_vision_answer(void);

#endif
