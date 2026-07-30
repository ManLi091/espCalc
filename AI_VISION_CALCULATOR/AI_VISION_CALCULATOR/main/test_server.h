#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <stddef.h>
#include <stdint.h>

// Starts the local test HTTP server. Call once, after WiFi is connected.
// Serves:
//   GET  /           a page showing the latest photo + a textarea to
//                     paste AI-style response text (using the ^{}, _{},
//                     \f{}{}, \sqrt{} markup) straight to the display,
//                     for testing the renderer without calling OpenAI.
//   GET  /photo.jpg  the latest captured photo (see test_server_set_photo)
//   POST /submit     raw text body -> passed straight to display_set_text()
void test_server_start(void);

// Copies jpg_buf (len bytes) into the server's own buffer so it can be
// served at /photo.jpg. Safe to call from the main task while the httpd
// task might be serving a previous photo — internally mutex-protected.
void test_server_set_photo(const uint8_t *jpg_buf, size_t len);

#endif
