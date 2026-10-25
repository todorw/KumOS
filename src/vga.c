#include "vga.h"
#include "gui.h"
#include "sched.h"
#include "irq.h"
#include <stdint.h>
#include <stddef.h>

static volatile uint16_t *const VGA_MEM = (uint16_t *)0xB8000;

#define ANSI_MAX_PARAMS 8

typedef struct {
    int      row, col;
    uint8_t  color;
    int      esc_state;
    int      params[ANSI_MAX_PARAMS];
    int      nparams;
    int      private_mode;
    uint8_t  fg, bg;
    int      bold, reverse;
    int      saved_row, saved_col;
    int      cursor_visible;
    int      alt_active;
    int      alt_row, alt_col;
    uint16_t shadow[VGA_WIDTH * VGA_HEIGHT];
    uint16_t alt[VGA_WIDTH * VGA_HEIGHT];
} console_t;

static console_t cons[VGA_TTYS];

static inline console_t *con_of(int tty) {
    if (tty < 0 || tty >= VGA_TTYS) tty = 0;
    return &cons[tty];
}

static inline console_t *cur(void) {
    return con_of(sched_current()->tty);
}

static inline volatile uint16_t *vmem(console_t *c) {
    return (c == &cons[0] && !gui_is_active()) ? VGA_MEM : (volatile uint16_t *)c->shadow;
}

const uint16_t *vga_tty_buffer(int tty) { return con_of(tty)->shadow; }

int vga_tty_cursor(int tty, int *row, int *col) {
    console_t *c = con_of(tty);
    *row = c->row; *col = c->col;
    return c->cursor_visible;
}

static const uint8_t ansi_color_map[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN,
    VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LIGHT_GREY
};
static const uint8_t ansi_bright_color_map[8] = {
    VGA_DARK_GREY, VGA_LIGHT_RED, VGA_LIGHT_GREEN, VGA_YELLOW,
    VGA_LIGHT_BLUE, VGA_LIGHT_MAGENTA, VGA_LIGHT_CYAN, VGA_WHITE
};

static inline uint16_t vga_entry(char ch, uint8_t color) {
    return (uint16_t)(uint8_t)ch | ((uint16_t)color << 8);
}

static inline uint8_t vga_color_make(vga_color fg, vga_color bg) {
    return (uint8_t)fg | ((uint8_t)bg << 4);
}

static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint8_t vga_font_buf[256 * 32];
static int     vga_font_saved = 0;

void vga_font_snapshot(void) {
    volatile uint8_t *plane = (volatile uint8_t *)0xA0000;

    outb(0x3C4, 0x02); uint8_t old_mm2 = inb(0x3C5);
    outb(0x3C4, 0x04); uint8_t old_mem_mode = inb(0x3C5);
    outb(0x3CE, 0x04); uint8_t old_read_map = inb(0x3CF);
    outb(0x3CE, 0x05); uint8_t old_gmode = inb(0x3CF);
    outb(0x3CE, 0x06); uint8_t old_misc = inb(0x3CF);

    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, (uint8_t)(old_mem_mode | 0x04));
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, (uint8_t)(old_gmode & ~0x10));
    outb(0x3CE, 0x06); outb(0x3CF, (uint8_t)(old_misc & ~0x0C));

    for (int i = 0; i < 256 * 32; i++) vga_font_buf[i] = plane[i];
    vga_font_saved = 1;

    outb(0x3C4, 0x02); outb(0x3C5, old_mm2);
    outb(0x3C4, 0x04); outb(0x3C5, old_mem_mode);
    outb(0x3CE, 0x04); outb(0x3CF, old_read_map);
    outb(0x3CE, 0x05); outb(0x3CF, old_gmode);
    outb(0x3CE, 0x06); outb(0x3CF, old_misc);
}

void vga_font_restore(void) {
    if (!vga_font_saved) return;
    volatile uint8_t *plane = (volatile uint8_t *)0xA0000;

    outb(0x3C4, 0x02); uint8_t old_mm2 = inb(0x3C5);
    outb(0x3C4, 0x04); uint8_t old_mem_mode = inb(0x3C5);
    outb(0x3CE, 0x04); uint8_t old_read_map = inb(0x3CF);
    outb(0x3CE, 0x05); uint8_t old_gmode = inb(0x3CF);
    outb(0x3CE, 0x06); uint8_t old_misc = inb(0x3CF);

    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, (uint8_t)(old_mem_mode | 0x04));
    outb(0x3CE, 0x05); outb(0x3CF, (uint8_t)(old_gmode & ~0x10));
    outb(0x3CE, 0x06); outb(0x3CF, (uint8_t)(old_misc & ~0x0C));

    for (int i = 0; i < 256 * 32; i++) plane[i] = vga_font_buf[i];

    outb(0x3C4, 0x02); outb(0x3C5, old_mm2);
    outb(0x3C4, 0x04); outb(0x3C5, old_mem_mode);
    outb(0x3CE, 0x04); outb(0x3CF, old_read_map);
    outb(0x3CE, 0x05); outb(0x3CF, old_gmode);
    outb(0x3CE, 0x06); outb(0x3CF, old_misc);
}

static void hw_cursor(console_t *c) {
    if (c != &cons[0] || gui_is_active()) return;
    uint16_t pos = (uint16_t)(c->row * VGA_WIDTH + c->col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_cursor(int x, int y) {
    console_t *c = cur();
    c->col = x; c->row = y;
    hw_cursor(c);
}

static void con_show_cursor(console_t *c, int show) {
    c->cursor_visible = show;
    if (c != &cons[0] || gui_is_active()) return;
    outb(0x3D4, 0x0A);
    outb(0x3D5, show ? 0x0E : 0x20);
}

int vga_cursor_visible(void) { return cur()->cursor_visible; }

static void con_goto(console_t *c, int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    c->row = row; c->col = col;
    hw_cursor(c);
}

static void con_fill(console_t *c, int start, int end) {
    volatile uint16_t *m = vmem(c);
    uint16_t e = vga_entry(' ', c->color);
    for (int i = start; i <= end; i++) m[i] = e;
}

static void con_scroll_up(console_t *c) {
    volatile uint16_t *m = vmem(c);
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) m[i] = m[i + VGA_WIDTH];
    con_fill(c, (VGA_HEIGHT - 1) * VGA_WIDTH, VGA_HEIGHT * VGA_WIDTH - 1);
}

static void con_scroll_down(console_t *c) {
    volatile uint16_t *m = vmem(c);
    for (int i = VGA_HEIGHT * VGA_WIDTH - 1; i >= VGA_WIDTH; i--) m[i] = m[i - VGA_WIDTH];
    con_fill(c, 0, VGA_WIDTH - 1);
}

static void con_clear(console_t *c) {
    con_fill(c, 0, VGA_WIDTH * VGA_HEIGHT - 1);
    con_goto(c, 0, 0);
}

static void con_reset(console_t *c) {
    c->row = c->col = 0;
    c->color = vga_color_make(VGA_LIGHT_GREY, VGA_BLACK);
    c->esc_state = 0;
    c->nparams = 0;
    c->private_mode = 0;
    c->fg = VGA_LIGHT_GREY; c->bg = VGA_BLACK;
    c->bold = c->reverse = 0;
    c->saved_row = c->saved_col = 0;
    c->cursor_visible = 1;
    c->alt_active = 0;
    con_clear(c);
}

void vga_tty_reset(int tty) {
    uint32_t f = irq_save();
    con_reset(con_of(tty));
    irq_restore(f);
}

static void con_apply_sgr(console_t *c) {
    if (c->nparams == 0) { c->params[0] = 0; c->nparams = 1; }
    for (int i = 0; i < c->nparams; i++) {
        int p = c->params[i];
        if (p == 0) { c->fg = VGA_LIGHT_GREY; c->bg = VGA_BLACK; c->bold = 0; c->reverse = 0; }
        else if (p == 1) c->bold = 1;
        else if (p == 22) c->bold = 0;
        else if (p == 7) c->reverse = 1;
        else if (p == 27) c->reverse = 0;
        else if (p >= 30 && p <= 37) c->fg = c->bold ? ansi_bright_color_map[p-30] : ansi_color_map[p-30];
        else if (p == 39) c->fg = VGA_LIGHT_GREY;
        else if (p >= 40 && p <= 47) c->bg = ansi_color_map[p-40];
        else if (p == 49) c->bg = VGA_BLACK;
        else if (p >= 90 && p <= 97) c->fg = ansi_bright_color_map[p-90];
        else if (p >= 100 && p <= 107) c->bg = ansi_bright_color_map[p-100];
    }
    c->color = c->reverse ? vga_color_make((vga_color)c->bg, (vga_color)c->fg)
                          : vga_color_make((vga_color)c->fg, (vga_color)c->bg);
}

static void con_alt_enter(console_t *c) {
    if (c->alt_active) return;
    volatile uint16_t *m = vmem(c);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) c->alt[i] = m[i];
    c->alt_row = c->row; c->alt_col = c->col;
    c->alt_active = 1;
    con_clear(c);
}

static void con_alt_exit(console_t *c) {
    if (!c->alt_active) return;
    volatile uint16_t *m = vmem(c);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) m[i] = c->alt[i];
    c->alt_active = 0;
    con_goto(c, c->alt_row, c->alt_col);
}

static void con_csi(console_t *c, char ch) {
    int p0 = c->params[0];
    int n  = p0 ? p0 : 1;
    switch (ch) {
        case 'm': con_apply_sgr(c); break;
        case 'H': case 'f': {
            int row = p0 ? p0 : 1;
            int col = (c->nparams > 1 && c->params[1]) ? c->params[1] : 1;
            con_goto(c, row - 1, col - 1);
            break;
        }
        case 'J':
            if (p0 == 1) con_fill(c, 0, c->row * VGA_WIDTH + c->col);
            else if (p0 == 2 || p0 == 3) con_fill(c, 0, VGA_HEIGHT * VGA_WIDTH - 1);
            else con_fill(c, c->row * VGA_WIDTH + c->col, VGA_HEIGHT * VGA_WIDTH - 1);
            break;
        case 'K':
            if (p0 == 1) con_fill(c, c->row * VGA_WIDTH, c->row * VGA_WIDTH + c->col);
            else if (p0 == 2) con_fill(c, c->row * VGA_WIDTH, c->row * VGA_WIDTH + VGA_WIDTH - 1);
            else con_fill(c, c->row * VGA_WIDTH + c->col, c->row * VGA_WIDTH + VGA_WIDTH - 1);
            break;
        case 'A': con_goto(c, c->row - n, c->col); break;
        case 'B': con_goto(c, c->row + n, c->col); break;
        case 'C': con_goto(c, c->row, c->col + n); break;
        case 'D': con_goto(c, c->row, c->col - n); break;
        case 'E': con_goto(c, c->row + n, 0); break;
        case 'F': con_goto(c, c->row - n, 0); break;
        case 'G': con_goto(c, c->row, n - 1); break;
        case 'S': for (int i = 0; i < n && i < VGA_HEIGHT; i++) con_scroll_up(c); break;
        case 'T': for (int i = 0; i < n && i < VGA_HEIGHT; i++) con_scroll_down(c); break;
        case 's': c->saved_row = c->row; c->saved_col = c->col; break;
        case 'u': con_goto(c, c->saved_row, c->saved_col); break;
        case 'h':
            if (c->private_mode && p0 == 25) con_show_cursor(c, 1);
            else if (c->private_mode && (p0 == 1049 || p0 == 47)) con_alt_enter(c);
            break;
        case 'l':
            if (c->private_mode && p0 == 25) con_show_cursor(c, 0);
            else if (c->private_mode && (p0 == 1049 || p0 == 47)) con_alt_exit(c);
            break;
        default: break;
    }
}

static void con_newline(console_t *c) {
    c->col = 0;
    if (++c->row >= VGA_HEIGHT) { con_scroll_up(c); c->row = VGA_HEIGHT - 1; }
}

static void con_putc(console_t *c, char ch) {
    if (c->esc_state == 1) {
        if (ch == '[') { c->esc_state = 2; c->nparams = 0; c->params[0] = 0; c->private_mode = 0; }
        else if (ch == ']') c->esc_state = 3;
        else if (ch == '7') { c->saved_row = c->row; c->saved_col = c->col; c->esc_state = 0; }
        else if (ch == '8') { con_goto(c, c->saved_row, c->saved_col); c->esc_state = 0; }
        else c->esc_state = 0;
        return;
    }
    if (c->esc_state == 3) {
        if ((uint8_t)ch == 7) c->esc_state = 0;
        else if ((uint8_t)ch == 27) c->esc_state = 4;
        return;
    }
    if (c->esc_state == 4) { c->esc_state = 0; return; }
    if (c->esc_state == 2) {
        if (ch == '?') { c->private_mode = 1; return; }
        if (ch >= '0' && ch <= '9') {
            if (c->nparams == 0) { c->nparams = 1; c->params[0] = 0; }
            int *p = &c->params[c->nparams - 1];
            if (*p < 10000) *p = *p * 10 + (ch - '0');
        } else if (ch == ';') {
            if (c->nparams == 0) { c->nparams = 1; c->params[0] = 0; }
            if (c->nparams < ANSI_MAX_PARAMS) c->params[c->nparams++] = 0;
        } else {
            if (c->nparams == 0) { c->nparams = 1; c->params[0] = 0; }
            con_csi(c, ch);
            c->esc_state = 0;
            c->private_mode = 0;
        }
        return;
    }
    if ((uint8_t)ch == 27) { c->esc_state = 1; return; }

    volatile uint16_t *m = vmem(c);
    if (ch == '\n') {
        con_newline(c);
    } else if (ch == '\r') {
        c->col = 0;
    } else if (ch == '\b') {
        if (c->col > 0) {
            c->col--;
            m[c->row * VGA_WIDTH + c->col] = vga_entry(' ', c->color);
        }
    } else if (ch == '\t') {
        int spaces = 4 - (c->col % 4);
        for (int i = 0; i < spaces; i++) con_putc(c, ' ');
        return;
    } else {
        m[c->row * VGA_WIDTH + c->col] = vga_entry(ch, c->color);
        if (++c->col >= VGA_WIDTH) con_newline(c);
    }
    hw_cursor(c);
}

void vga_init(void) {
    for (int t = 0; t < VGA_TTYS; t++) con_reset(&cons[t]);
}

void vga_clear(void) {
    uint32_t f = irq_save();
    con_clear(cur());
    irq_restore(f);
}

void vga_set_color(vga_color fg, vga_color bg) {
    cur()->color = vga_color_make(fg, bg);
}

void vga_scroll(void) {
    uint32_t f = irq_save();
    console_t *c = cur();
    con_scroll_up(c);
    c->row = VGA_HEIGHT - 1;
    irq_restore(f);
}

void vga_putchar(char ch) {
    uint32_t f = irq_save();
    con_putc(cur(), ch);
    irq_restore(f);
}

void vga_puts(const char *str) {
    uint32_t f = irq_save();
    console_t *c = cur();
    while (*str) con_putc(c, *str++);
    irq_restore(f);
}

void vga_puts_at(const char *str, int x, int y, vga_color fg, vga_color bg) {
    if (y < 0 || y >= VGA_HEIGHT) return;
    volatile uint16_t *m = vmem(&cons[0]);
    uint8_t color = vga_color_make(fg, bg);
    for (int cx = x; *str && cx < VGA_WIDTH; cx++, str++)
        if (cx >= 0) m[y * VGA_WIDTH + cx] = vga_entry(*str, color);
}

void vga_draw_box(int x, int y, int w, int h, vga_color fg, vga_color bg) {
    if (w < 2 || h < 2 || x < 0 || y < 0 || x + w > VGA_WIDTH || y + h > VGA_HEIGHT) return;
    volatile uint16_t *m = vmem(&cons[0]);
    uint8_t color = vga_color_make(fg, bg);

    m[y * VGA_WIDTH + x] = vga_entry((char)VGA_CH_TL, color);
    m[y * VGA_WIDTH + x + w - 1] = vga_entry((char)VGA_CH_TR, color);
    m[(y+h-1) * VGA_WIDTH + x] = vga_entry((char)VGA_CH_BL, color);
    m[(y+h-1) * VGA_WIDTH + x + w - 1] = vga_entry((char)VGA_CH_BR, color);
    for (int i = 1; i < w-1; i++) {
        m[y * VGA_WIDTH + x + i] = vga_entry((char)VGA_CH_HLINE, color);
        m[(y+h-1) * VGA_WIDTH + x + i] = vga_entry((char)VGA_CH_HLINE, color);
    }
    for (int i = 1; i < h-1; i++) {
        m[(y+i) * VGA_WIDTH + x] = vga_entry((char)VGA_CH_VLINE, color);
        m[(y+i) * VGA_WIDTH + x + w - 1] = vga_entry((char)VGA_CH_VLINE, color);
    }
}

void vga_fill_rect(int x, int y, int w, int h, char ch, vga_color fg, vga_color bg) {
    volatile uint16_t *m = vmem(&cons[0]);
    uint8_t color = vga_color_make(fg, bg);
    for (int row = y < 0 ? 0 : y; row < y+h && row < VGA_HEIGHT; row++)
        for (int col = x < 0 ? 0 : x; col < x+w && col < VGA_WIDTH; col++)
            m[row * VGA_WIDTH + col] = vga_entry(ch, color);
}

int vga_get_col(void) { return cur()->col; }
int vga_get_row(void) { return cur()->row; }

void vga_goto(int row, int col) {
    con_goto(cur(), row, col);
}

void vga_put_hex(uint32_t val) {
    char buf[11];
    const char *hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[10] = 0;
    vga_puts(buf);
}

void vga_put_dec(uint32_t val) {
    if (val == 0) { vga_putchar('0'); return; }
    char buf[12]; int i = 10;
    buf[11] = 0;
    while (val > 0) { buf[i--] = '0' + (val % 10); val /= 10; }
    vga_puts(&buf[i+1]);
}
