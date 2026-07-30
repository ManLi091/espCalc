// display.c -> GMG12864-06D (ST7565R, 128x64) driver via u8g2, plus a
// small math-markup parser/layout engine so AI responses render as real
// typeset math: raised exponents, an actual fraction bar, a drawn root
// symbol — not ASCII approximations.
//
// Markup understood (see OPENAI_SYSTEM_PROMPT in config.h — this is
// exactly what the AI is told to produce):
//   ^{...}        exponent (superscript)
//   _{...}        subscript
//   \f{num}{den}  fraction
//   \sqrt{...}    square root
// Content inside {} is plain text only — no nesting. This keeps the
// parser a single flat pass per line instead of a recursive one.

#include "display.h"
#include "config.h"
#include "u8g2_hal.h"
#include "u8g2.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "DISPLAY";

// Guards s_u8g2 / s_lines / s_pages / s_current_page. Only matters
// because test_server.c can now call display_set_text() from the httpd
// task while main.c's button loop is also touching the display from the
// main task — without this, two tasks issuing SPI transactions to the
// same u8g2 instance at once could corrupt the framebuffer or the SPI
// transaction stream.
static SemaphoreHandle_t s_display_mutex;

static u8g2_t s_u8g2;
static const uint8_t *FONT_NORMAL = u8g2_font_6x10_tf; // body text, numerators/denominators
static const uint8_t *FONT_SMALL  = u8g2_font_4x6_tf;  // superscripts/subscripts

// Cached font metrics (pixels), computed once in display_init().
static int s_normal_ascent, s_normal_descent, s_normal_line_h;
static int s_small_ascent, s_small_descent;

#define FRAC_PAD 2      // horizontal padding either side of a fraction's widest part
#define FRAC_GAP 2      // vertical gap between numerator/bar/denominator
#define SQRT_HOOK_W 5   // width reserved for the radical hook before the bar starts
#define LINE_GAP 1      // vertical gap between lines on a page

typedef enum {
    SEG_TEXT,
    SEG_SUP,
    SEG_SUB,
    SEG_SQRT,
    SEG_FRAC
} seg_type_t;

typedef struct {
    seg_type_t type;
    char text[MARKUP_CONTENT_MAXLEN + 1];     // TEXT / SUP / SUB / SQRT content
    char frac_num[MARKUP_CONTENT_MAXLEN + 1]; // FRAC only
    char frac_den[MARKUP_CONTENT_MAXLEN + 1]; // FRAC only
    int width_px;
} segment_t;

typedef struct {
    segment_t segments[MAX_SEGMENTS_PER_LINE];
    uint8_t segment_count;
    int height_px;
    bool has_frac;
    bool has_sup_sub;
} render_line_t;

typedef struct {
    uint16_t start_line;
    uint16_t end_line; // inclusive
} page_range_t;

// Heap-allocated (PSRAM) — MAX_LINES * sizeof(render_line_t) is too big
// to comfortably keep as a static/global (that always lands in internal
// SRAM regardless of CONFIG_SPIRAM_USE_MALLOC, which only affects
// malloc() calls). Allocated once, reused across captures.
static render_line_t *s_lines = NULL;
static uint16_t s_line_count = 0;

static page_range_t s_pages[MAX_PAGES];
static uint16_t s_page_count = 0;
static uint16_t s_current_page = 0;

// ---------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------

static void copy_bounded(char *dst, size_t dst_cap, const char *src, size_t len) {
    if (len > dst_cap - 1) len = dst_cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Finds the next '}' at or after `start`. Content is plain text only, so
// no brace-nesting to worry about. Returns -1 if none found before end.
static int find_close_brace(const char *s, int start, int len) {
    for (int i = start; i < len; i++) {
        if (s[i] == '}') return i;
    }
    return -1;
}

static segment_t *push_segment(render_line_t *line, seg_type_t type) {
    if (line->segment_count >= MAX_SEGMENTS_PER_LINE) return NULL;
    segment_t *seg = &line->segments[line->segment_count++];
    memset(seg, 0, sizeof(*seg));
    seg->type = type;
    return seg;
}

static void parse_line(const char *raw, render_line_t *out) {
    memset(out, 0, sizeof(*out));
    int len = (int)strlen(raw);
    int i = 0;

    while (i < len && out->segment_count < MAX_SEGMENTS_PER_LINE) {

        if (raw[i] == '^' && i + 1 < len && raw[i + 1] == '{') {
            int close = find_close_brace(raw, i + 2, len);
            if (close >= 0) {
                segment_t *seg = push_segment(out, SEG_SUP);
                if (seg) copy_bounded(seg->text, sizeof(seg->text), raw + i + 2, close - (i + 2));
                i = close + 1;
                continue;
            }
        }

        if (raw[i] == '_' && i + 1 < len && raw[i + 1] == '{') {
            int close = find_close_brace(raw, i + 2, len);
            if (close >= 0) {
                segment_t *seg = push_segment(out, SEG_SUB);
                if (seg) copy_bounded(seg->text, sizeof(seg->text), raw + i + 2, close - (i + 2));
                i = close + 1;
                continue;
            }
        }

        if (i + 3 <= len && strncmp(raw + i, "\\f{", 3) == 0) {
            int close1 = find_close_brace(raw, i + 3, len);
            if (close1 >= 0 && close1 + 1 < len && raw[close1 + 1] == '{') {
                int close2 = find_close_brace(raw, close1 + 2, len);
                if (close2 >= 0) {
                    segment_t *seg = push_segment(out, SEG_FRAC);
                    if (seg) {
                        copy_bounded(seg->frac_num, sizeof(seg->frac_num), raw + i + 3, close1 - (i + 3));
                        copy_bounded(seg->frac_den, sizeof(seg->frac_den), raw + close1 + 2, close2 - (close1 + 2));
                    }
                    i = close2 + 1;
                    continue;
                }
            }
        }

        if (i + 6 <= len && strncmp(raw + i, "\\sqrt{", 6) == 0) {
            int close = find_close_brace(raw, i + 6, len);
            if (close >= 0) {
                segment_t *seg = push_segment(out, SEG_SQRT);
                if (seg) copy_bounded(seg->text, sizeof(seg->text), raw + i + 6, close - (i + 6));
                i = close + 1;
                continue;
            }
        }

        // Plain text run: consume until the next markup trigger or end of line.
        int start = i;
        while (i < len) {
            if (raw[i] == '^' && i + 1 < len && raw[i + 1] == '{') break;
            if (raw[i] == '_' && i + 1 < len && raw[i + 1] == '{') break;
            if (i + 3 <= len && strncmp(raw + i, "\\f{", 3) == 0) break;
            if (i + 6 <= len && strncmp(raw + i, "\\sqrt{", 6) == 0) break;
            i++;
        }
        if (i > start) {
            segment_t *seg = push_segment(out, SEG_TEXT);
            if (seg) copy_bounded(seg->text, sizeof(seg->text), raw + start, i - start);
        } else {
            // Malformed markup trigger with no closing brace — treat this
            // one character as literal text and move on rather than loop.
            segment_t *seg = push_segment(out, SEG_TEXT);
            if (seg) copy_bounded(seg->text, sizeof(seg->text), raw + i, 1);
            i++;
        }
    }
}

// ---------------------------------------------------------------------
// Measuring
// ---------------------------------------------------------------------

static int str_w(const uint8_t *font, const char *s) {
    u8g2_SetFont(&s_u8g2, font);
    return u8g2_GetStrWidth(&s_u8g2, s);
}

static void measure_line(render_line_t *line) {
    int total_w = 0;
    line->has_frac = false;
    line->has_sup_sub = false;

    for (int i = 0; i < line->segment_count; i++) {
        segment_t *seg = &line->segments[i];
        switch (seg->type) {
            case SEG_TEXT:
                seg->width_px = str_w(FONT_NORMAL, seg->text);
                break;
            case SEG_SUP:
            case SEG_SUB:
                seg->width_px = str_w(FONT_SMALL, seg->text);
                line->has_sup_sub = true;
                break;
            case SEG_SQRT: {
                int inner_w = str_w(FONT_NORMAL, seg->text);
                seg->width_px = SQRT_HOOK_W + inner_w + 1;
                break;
            }
            case SEG_FRAC: {
                int num_w = str_w(FONT_NORMAL, seg->frac_num);
                int den_w = str_w(FONT_NORMAL, seg->frac_den);
                int w = num_w > den_w ? num_w : den_w;
                seg->width_px = w + 2 * FRAC_PAD;
                line->has_frac = true;
                break;
            }
        }
        total_w += seg->width_px;
    }

    if (line->has_frac) {
        line->height_px = 2 * s_normal_line_h + 2 * FRAC_GAP + 1; // numerator + bar + denominator
    } else if (line->has_sup_sub) {
        line->height_px = s_normal_line_h + (s_small_ascent / 2) + 1;
    } else {
        line->height_px = s_normal_line_h;
    }
    (void)total_w; // wrapping across segments isn't implemented — see chat notes on this limitation
}

// ---------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------

static void draw_sqrt(int x, int baseline_y, const char *inner) {
    // A simple radical hook: short down-stroke, then up-stroke to the
    // bar, then the bar itself over the radicand.
    int inner_w = str_w(FONT_NORMAL, inner);
    u8g2_DrawLine(&s_u8g2, x, baseline_y - 2, x + 2, baseline_y + 1);
    u8g2_DrawLine(&s_u8g2, x + 2, baseline_y + 1, x + SQRT_HOOK_W, baseline_y - s_normal_ascent);
    u8g2_DrawHLine(&s_u8g2, x + SQRT_HOOK_W, baseline_y - s_normal_ascent, inner_w + 1);
    u8g2_SetFont(&s_u8g2, FONT_NORMAL);
    u8g2_DrawStr(&s_u8g2, x + SQRT_HOOK_W, baseline_y, inner);
}

static void draw_frac(int x, int y_top, segment_t *seg) {
    int num_w = str_w(FONT_NORMAL, seg->frac_num);
    int den_w = str_w(FONT_NORMAL, seg->frac_den);
    int slot_w = seg->width_px;

    int bar_y = y_top + s_normal_line_h + FRAC_GAP / 2;

    u8g2_SetFont(&s_u8g2, FONT_NORMAL);
    u8g2_DrawStr(&s_u8g2, x + (slot_w - num_w) / 2, y_top + s_normal_ascent, seg->frac_num);
    u8g2_DrawHLine(&s_u8g2, x, bar_y, slot_w);
    u8g2_DrawStr(&s_u8g2, x + (slot_w - den_w) / 2, bar_y + FRAC_GAP + s_normal_ascent, seg->frac_den);
}

static void render_line(render_line_t *line, int y_top) {
    int x = 0;

    if (line->has_frac) {
        // Plain-text segments on a fraction line sit at the bar's level;
        // fraction segments lay out their own two rows around it.
        int bar_baseline = y_top + s_normal_line_h + FRAC_GAP / 2 + s_normal_ascent / 2;
        for (int i = 0; i < line->segment_count; i++) {
            segment_t *seg = &line->segments[i];
            switch (seg->type) {
                case SEG_FRAC:
                    draw_frac(x, y_top, seg);
                    break;
                case SEG_TEXT:
                    u8g2_SetFont(&s_u8g2, FONT_NORMAL);
                    u8g2_DrawStr(&s_u8g2, x, bar_baseline, seg->text);
                    break;
                case SEG_SQRT:
                    draw_sqrt(x, bar_baseline, seg->text);
                    break;
                case SEG_SUP:
                    u8g2_SetFont(&s_u8g2, FONT_SMALL);
                    u8g2_DrawStr(&s_u8g2, x, bar_baseline - s_normal_line_h / 2, seg->text);
                    break;
                case SEG_SUB:
                    u8g2_SetFont(&s_u8g2, FONT_SMALL);
                    u8g2_DrawStr(&s_u8g2, x, bar_baseline + s_small_ascent / 2, seg->text);
                    break;
            }
            x += seg->width_px;
        }
        return;
    }

    int baseline_y = y_top + s_normal_ascent;
    for (int i = 0; i < line->segment_count; i++) {
        segment_t *seg = &line->segments[i];
        switch (seg->type) {
            case SEG_TEXT:
                u8g2_SetFont(&s_u8g2, FONT_NORMAL);
                u8g2_DrawStr(&s_u8g2, x, baseline_y, seg->text);
                break;
            case SEG_SUP:
                u8g2_SetFont(&s_u8g2, FONT_SMALL);
                u8g2_DrawStr(&s_u8g2, x, baseline_y - s_normal_line_h / 2, seg->text);
                break;
            case SEG_SUB:
                u8g2_SetFont(&s_u8g2, FONT_SMALL);
                u8g2_DrawStr(&s_u8g2, x, baseline_y + s_small_ascent / 2, seg->text);
                break;
            case SEG_SQRT:
                draw_sqrt(x, baseline_y, seg->text);
                break;
            case SEG_FRAC:
                draw_frac(x, y_top, seg); // shouldn't normally happen (has_frac would be true) — safe fallback
                break;
        }
        x += seg->width_px;
    }
}

static void render_page(uint16_t page_idx) {
    if (page_idx >= s_page_count || !s_lines) return;
    u8g2_ClearBuffer(&s_u8g2);

    int y = 0;
    page_range_t pr = s_pages[page_idx];
    for (uint16_t l = pr.start_line; l <= pr.end_line && l < s_line_count; l++) {
        render_line(&s_lines[l], y);
        y += s_lines[l].height_px + LINE_GAP;
    }

    u8g2_SendBuffer(&s_u8g2);
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

void display_init(void) {
    if (!s_display_mutex) {
        // Recursive: display_set_text()'s error path calls
        // display_show_message() internally, which would deadlock a
        // plain mutex.
        s_display_mutex = xSemaphoreCreateRecursiveMutex();
    }

    u8g2_hal_init(&s_u8g2);

    u8g2_SetFont(&s_u8g2, FONT_NORMAL);
    s_normal_ascent = u8g2_GetAscent(&s_u8g2);
    s_normal_descent = -u8g2_GetDescent(&s_u8g2); // u8g2 returns descent as negative
    s_normal_line_h = s_normal_ascent + s_normal_descent + 1;

    u8g2_SetFont(&s_u8g2, FONT_SMALL);
    s_small_ascent = u8g2_GetAscent(&s_u8g2);
    s_small_descent = -u8g2_GetDescent(&s_u8g2);

    if (!s_lines) {
        s_lines = (render_line_t *)heap_caps_malloc(MAX_LINES * sizeof(render_line_t), MALLOC_CAP_SPIRAM);
        if (!s_lines) {
            ESP_LOGE(TAG, "Failed to allocate line buffer in PSRAM (%u bytes)",
                     (unsigned)(MAX_LINES * sizeof(render_line_t)));
        }
    }
}

void display_clear(void) {
    xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
    xSemaphoreGiveRecursive(s_display_mutex);
}

void display_show_message(const char *msg) {
    xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, FONT_NORMAL);
    u8g2_DrawStr(&s_u8g2, 0, u8g2_GetAscent(&s_u8g2), msg);
    u8g2_SendBuffer(&s_u8g2);
    xSemaphoreGiveRecursive(s_display_mutex);
}

void display_set_text(const char *text) {
    xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);

    if (!s_lines) {
        display_show_message("ERROR: NO LINE BUFFER");
        xSemaphoreGiveRecursive(s_display_mutex);
        return;
    }

    // Split on '\n' into individual lines, parse + measure each.
    s_line_count = 0;
    const char *p = text;
    while (*p && s_line_count < MAX_LINES) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        char linebuf[256];
        if (len > (int)sizeof(linebuf) - 1) len = sizeof(linebuf) - 1;
        memcpy(linebuf, p, len);
        linebuf[len] = '\0';

        parse_line(linebuf, &s_lines[s_line_count]);
        measure_line(&s_lines[s_line_count]);
        s_line_count++;

        if (!nl) break;
        p = nl + 1;
    }

    // Pack lines into pages by cumulative pixel height.
    s_page_count = 0;
    uint16_t line_i = 0;
    while (line_i < s_line_count && s_page_count < MAX_PAGES) {
        uint16_t start = line_i;
        int used_h = 0;
        while (line_i < s_line_count) {
            int h = s_lines[line_i].height_px + LINE_GAP;
            if (used_h + h > SCREEN_HEIGHT_PX && line_i > start) break;
            used_h += h;
            line_i++;
        }
        s_pages[s_page_count].start_line = start;
        s_pages[s_page_count].end_line = line_i - 1;
        s_page_count++;
    }
    if (s_page_count == 0 && s_line_count > 0) {
        // Single very tall line (e.g. a huge fraction) — still show it as its own page.
        s_pages[0].start_line = 0;
        s_pages[0].end_line = s_line_count - 1;
        s_page_count = 1;
    }

    s_current_page = 0;
    render_page(s_current_page);

    xSemaphoreGiveRecursive(s_display_mutex);
}

void display_scroll_up(void) {
    xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    if (s_current_page > 0) {
        s_current_page--;
        render_page(s_current_page);
    }
    xSemaphoreGiveRecursive(s_display_mutex);
}


void display_scroll_down(void) {
    xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    if (s_current_page + 1 < s_page_count) {
        s_current_page++;
        render_page(s_current_page);
    }
    xSemaphoreGiveRecursive(s_display_mutex);
}
