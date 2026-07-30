#ifndef DISPLAY_H
#define DISPLAY_H

// Public API unchanged from the character-LCD version, so main.c doesn't
// need to change — only the implementation (now u8g2 + SPI) does.

void display_init(void);
void display_clear(void);
void display_show_message(const char *msg);
void display_set_text(const char *text);
void display_scroll_up(void);
void display_scroll_down(void);

#endif
