// AI Vision Calculator — simplified firmware
//
// config.h -> Pin definitions and tunables for all .c files
//
// Derived from the original "ChatGPT Hardware Hack" project by
// Jonas Heselschwerdt (CC BY-NC 4.0), stripped down to: deep sleep,
// 3 buttons, camera capture, OpenAI vision, LCD scroll display.

#ifndef CONFIG_H
#define CONFIG_H

#define firmware_version " AI Vision Calc v0.1 "   // must be exactly 20 chars — shown on boot

// ===========================================================
// LCD pins — GMG12864-06D, ST7565R controller, SPI interface.
// Replaces the earlier 20x4 character LCD entirely.
// ===========================================================
// None of these need to be RTC-capable — only the button matrix's
// ROW_A/COL_A do (see below). These are in the ESP32-S3's higher GPIO
// range specifically so they don't compete with the low-numbered pins
// the camera and buttons need. Still worth double-checking against your
// exact module's datasheet: GPIO 26-32 (and sometimes more) are
// physically wired to octal PSRAM/flash on N16R8-style modules and are
// not available for anything else.
#define LCD_CS_GPIO  38   // chip select
#define LCD_RES_GPIO 39   // reset (labeled "RSE" on the board)
#define LCD_DC_GPIO  40   // data/command (labeled "RS" on the board)
#define LCD_SCL_GPIO 41   // SPI clock
#define LCD_SI_GPIO  42   // SPI MOSI (labeled "SI" on the board)
// VDD -> 3.3V, VSS -> GND, A (LED anode) -> 3.3V through a current-
// limiting resistor, K (LED cathode) -> GND. No GPIO needed for the
// backlight unless you want software on/off or dimming control.

#define PE 4   // power latch — must stay held high to keep the board powered,
               // including during deep sleep (deep sleep keeps the rail up;
               // only a hardware latch release would cut power entirely)

// ===========================================================
// Button matrix pins — PLACEHOLDERS, you said you'd fill these in.
// ===========================================================
// IMPORTANT: as flagged in chat, the camera (per camera_pins.h's current
// ESP32S3_EYE mapping) occupies GPIO 4-13 and 15-18 — basically all of
// the low range except 14 and 19-21. Only ROW_A and COL_A actually need
// to be in the RTC-capable 0-21 range (ROW_A because its output level
// is held through deep sleep, COL_A because it's the wake source) —
// ROW_B and COL_B can be any free GPIO, so they're placed alongside the
// LCD pins below instead of fighting for the scarce low range.
//
// 2 rows (outputs) x 2 columns (inputs) = 3 usable button positions:
//   ROW_A x COL_A -> capture   (also the button that wakes from deep sleep)
//   ROW_A x COL_B -> scroll up
//   ROW_B x COL_A -> scroll down
//   ROW_B x COL_B -> unused/spare
#define BTN_ROW_A_GPIO 14   // placeholder — RTC-capable, held low through deep sleep
#define BTN_COL_A_GPIO 21   // placeholder — RTC-capable, also the deep-sleep wake pin
#define BTN_ROW_B_GPIO 1    // placeholder — no RTC requirement
#define BTN_COL_B_GPIO 2    // placeholder — no RTC requirement

// ===========================================================
// Display geometry (128x64 graphical LCD) / typeset math rendering
// ===========================================================
// Lines are laid out with variable height now (a line containing a
// fraction is taller than a plain text line), so pagination works off
// actual measured pixel heights rather than a fixed character grid.
#define SCREEN_WIDTH_PX 128
#define SCREEN_HEIGHT_PX 64

#define MAX_ANSWER_CHARS 4000        // hard cap on the AI response we'll store/display
#define MAX_LINES 100                // max newline-separated lines we'll parse
#define MAX_SEGMENTS_PER_LINE 6      // plain-text/sup/sub/frac/sqrt segments per line
#define MARKUP_CONTENT_MAXLEN 31     // max chars inside a single {...}
#define MAX_PAGES 30

// ===========================================================
// Behaviour tuning
// ===========================================================
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define INACTIVITY_SLEEP_MS 60000    // no button press for this long -> back to deep sleep
#define BUTTON_POLL_MS 30            // polling interval while awake
#define BUTTON_DEBOUNCE_MS 150       // minimum time between accepted presses of the same button

// ===========================================================
// OpenAI vision request settings
// ===========================================================
#define OPENAI_VISION_MODEL "gpt-4o"   // must be a vision-capable model

// The fixed question sent with every photo (no keypad to type a custom
// one anymore). Change this if you want a different default behaviour.
#define OPENAI_VISION_USER_PROMPT \
    "Solve or explain what's shown in this image."

// Sent as the system message on every request. This defines the exact
// markup display.c's parser understands, so the AI's response can be
// rendered as real typeset math (raised exponents, an actual fraction
// bar, a drawn root symbol) rather than plain-text approximations.
#define OPENAI_SYSTEM_PROMPT \
    "You are running on a small 128x64 pixel graphical LCD with a math " \
    "renderer. Reply in plain 7-bit ASCII only, under 4000 characters " \
    "total, no markdown, no Unicode. For math, use this exact markup so " \
    "it renders as real typeset math: exponent as ^{...} right after its " \
    "base, e.g. x^{2}. Subscript as _{...}, e.g. x_{1}. Fraction as " \
    "\\f{numerator}{denominator}, e.g. \\f{a+b}{c}. Square root as " \
    "\\sqrt{...}, e.g. \\sqrt{x+1}. The content inside every {} must be " \
    "plain text only - never put another ^{}, _{}, \\f{}{}, or \\sqrt{} " \
    "inside one of these braces, and keep each {...} under 30 " \
    "characters. Put each step of a calculation on its own line, and " \
    "keep each line short (well under 20 visible characters) since " \
    "fractions and roots take extra width. Do not use tables."

#endif
