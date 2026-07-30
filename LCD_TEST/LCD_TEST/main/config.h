#ifndef CONFIG_H
#define CONFIG_H

// ===========================================================
// LCD pins — GMG12864-06D, ST7565R controller, SPI interface.
// Same placeholders as the main project. Change these to your real
// wiring before flashing.
// ===========================================================
#define LCD_CS_GPIO  38   // chip select
#define LCD_RES_GPIO 39   // reset (labeled "RSE" on the board)
#define LCD_DC_GPIO  40   // data/command (labeled "RS" on the board)
#define LCD_SCL_GPIO 41   // SPI clock
#define LCD_SI_GPIO  42   // SPI MOSI (labeled "SI" on the board)
// VDD -> 3.3V, VSS -> GND, A (LED anode) -> 3.3V through a resistor,
// K (LED cathode) -> GND.

// ===========================================================
// Display geometry / typeset math rendering — same as the main project.
// ===========================================================
#define SCREEN_WIDTH_PX 128
#define SCREEN_HEIGHT_PX 64

#define MAX_ANSWER_CHARS 4000
#define MAX_LINES 100
#define MAX_SEGMENTS_PER_LINE 6
#define MARKUP_CONTENT_MAXLEN 31
#define MAX_PAGES 30

#endif
