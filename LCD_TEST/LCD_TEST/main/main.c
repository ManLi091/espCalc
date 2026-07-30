// main.c -> Standalone LCD test. No WiFi, no camera, no buttons — just
// verifies the GMG12864-06D wiring/SPI/controller and exercises the same
// display.c math-markup renderer used in the full project, so you can
// debug the display in complete isolation.
//
// Cycles forever: a one-line sanity message -> a plain multi-line block
// -> a math-markup block that auto-scrolls through all its pages.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"

static const char *PLAIN_TEXT_TEST =
    "Plain text test\n"
    "Line 1 of 4\n"
    "Line 2 of 4\n"
    "Line 3 of 4\n"
    "Line 4 of 4\n"
    "(no math markup\n"
    "on this screen)";

static const char *MATH_MARKUP_TEST =
    "Math markup test\n"
    "\n"
    "Exponent: x^{2}+1\n"
    "Subscript: a_{1}+a_{2}\n"
    "Fraction: \\f{a+b}{c}\n"
    "Root: \\sqrt{x+1}\n"
    "Mixed: y=\\f{1}{2}x^{2}\n"
    "Nested-ish: \\sqrt{a^{2}}\n"
    "\n"
    "-- page break test --\n"
    "Marker line A\n"
    "Marker line B\n"
    "Marker line C\n"
    "Marker line D\n"
    "Marker line E\n"
    "\n"
    "End of test content.\n"
    "If you can read all\n"
    "of this across pages,\n"
    "the renderer works.";

void app_main(void) {
    display_init();

    while (1) {
        display_show_message("LCD WIRING TEST - OK");
        vTaskDelay(pdMS_TO_TICKS(2000));

        display_set_text(PLAIN_TEXT_TEST);
        vTaskDelay(pdMS_TO_TICKS(3000));

        display_set_text(MATH_MARKUP_TEST);
        // Auto-scroll through every page. display_scroll_down() is a
        // harmless no-op once you're already on the last page, so it's
        // safe to just call it more times than there are pages.
        for (int i = 0; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(2500));
            display_scroll_down();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
