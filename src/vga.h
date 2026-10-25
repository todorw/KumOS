#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW        = 14,
    VGA_WHITE         = 15,
} vga_color;

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_TTYS   3

/* CP437 box-drawing / shading glyphs (raw font code points, not ASCII) */
#define VGA_CH_HLINE   0xC4  /* ─ */
#define VGA_CH_VLINE   0xB3  /* │ */
#define VGA_CH_TL      0xDA  /* ┌ */
#define VGA_CH_TR      0xBF  /* ┐ */
#define VGA_CH_BL      0xC0  /* └ */
#define VGA_CH_BR      0xD9  /* ┘ */
#define VGA_CH_TTEE    0xC2  /* ┬ */
#define VGA_CH_BTEE    0xC1  /* ┴ */
#define VGA_CH_LTEE    0xC3  /* ├ */
#define VGA_CH_RTEE    0xB4  /* ┤ */
#define VGA_CH_CROSS   0xC5  /* ┼ */
#define VGA_CH_SHADE1  0xB0  /* ░ */
#define VGA_CH_SHADE2  0xB1  /* ▒ */
#define VGA_CH_SHADE3  0xB2  /* ▓ */
#define VGA_CH_BLOCK   0xDB  /* █ */

void vga_init(void);
void vga_clear(void);
void vga_set_color(vga_color fg, vga_color bg);
void vga_putchar(char c);
void vga_puts(const char *str);
void vga_puts_at(const char *str, int x, int y, vga_color fg, vga_color bg);
void vga_set_cursor(int x, int y);
void vga_goto(int row, int col);
void vga_draw_box(int x, int y, int w, int h, vga_color fg, vga_color bg);
void vga_fill_rect(int x, int y, int w, int h, char ch, vga_color fg, vga_color bg);
int  vga_get_col(void);
int  vga_get_row(void);
void vga_scroll(void);
void vga_put_hex(uint32_t val);
void vga_put_dec(uint32_t val);
void vga_font_snapshot(void);
void vga_font_restore(void);
int  vga_cursor_visible(void);
const uint16_t *vga_tty_buffer(int tty);
int  vga_tty_cursor(int tty, int *row, int *col);
void vga_tty_reset(int tty);

#endif