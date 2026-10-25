#include "gui.h"
#include "font8x8.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "rtc.h"
#include "kstring.h"
#include "fat12.h"
#include "kmalloc.h"
#include "paging.h"
#include "sched.h"
#include "users.h"
#include "signal.h"
#include "pkg.h"
#include "pkgnet.h"
#include "serial.h"
#include "elf.h"
#include "vfs.h"
#include "net.h"
#include "speaker.h"
#include "browser.h"
#include "power.h"
#include <stdint.h>

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw_(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw_(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outl_(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl_(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

static void set_mode3h(void) {
    outb(0x3C2, 0x67);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3C4, 0x01); outb(0x3C5, 0x00);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);
    static const uint8_t crtc3[25] = {
        0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,
        0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,0x9C,0x0E,
        0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF
    };
    for (int i = 0; i < 25; i++) {
        outb(0x3D4, (uint8_t)i); outb(0x3D5, crtc3[i]);
    }
    static const uint8_t gc3[9] = {
        0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
    };
    for (int i = 0; i < 9; i++) {
        outb(0x3CE, (uint8_t)i); outb(0x3CF, gc3[i]);
    }
    inb(0x3DA);
    static const uint8_t ac3[21] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
        0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
        0x0C,0x00,0x0F,0x08,0x00
    };
    for (int i = 0; i < 21; i++) {
        outb(0x3C0, (uint8_t)i); outb(0x3C0, ac3[i]);
    }
    outb(0x3C0, 0x20);
}

#define VBE_IOPORT_INDEX 0x01CE
#define VBE_IOPORT_DATA   0x01CF
#define VBE_IDX_ID        0
#define VBE_IDX_XRES      1
#define VBE_IDX_YRES      2
#define VBE_IDX_BPP       3
#define VBE_IDX_ENABLE    4
#define VBE_DISABLED      0x00
#define VBE_ENABLED       0x01
#define VBE_LFB_ENABLED   0x40

static void vbe_write(uint16_t idx, uint16_t val) {
    outw_(VBE_IOPORT_INDEX, idx);
    outw_(VBE_IOPORT_DATA, val);
}
static uint16_t vbe_read(uint16_t idx) {
    outw_(VBE_IOPORT_INDEX, idx);
    return inw_(VBE_IOPORT_DATA);
}

static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl_(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) | ((uint32_t)fn<<8) | (reg & 0xFC));
    return inl_(0xCFC);
}

static uint32_t find_vbe_fb_phys(void) {
    for (uint16_t bd = 0; bd < 256*32; bd++) {
        uint8_t bus = (uint8_t)(bd/32), dev = (uint8_t)(bd%32);
        uint32_t id = pci_cfg_read(bus, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0x1234 && ((id>>16) & 0xFFFF) == 0x1111) {
            uint32_t bar0 = pci_cfg_read(bus, dev, 0, 0x10);
            return bar0 & ~0xFu;
        }
    }
    return 0;
}

static int gui_active = 0;
int gui_is_active(void) { return gui_active; }

static uint32_t fb_phys_addr = 0;
static uint32_t fb_pitch     = 0;
static uint8_t *fb_ptr       = 0;

static int set_vbe_mode(uint16_t width, uint16_t height, uint16_t bpp) {
    if (!fb_phys_addr) {
        fb_phys_addr = find_vbe_fb_phys();
        if (!fb_phys_addr) return -1;
    }
    uint16_t id = vbe_read(VBE_IDX_ID);
    if (id < 0xB0C0 || id > 0xB0C6) return -1;

    vbe_write(VBE_IDX_ENABLE, VBE_DISABLED);
    vbe_write(VBE_IDX_XRES, width);
    vbe_write(VBE_IDX_YRES, height);
    vbe_write(VBE_IDX_BPP,  bpp);
    vbe_write(VBE_IDX_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);

    fb_pitch = (uint32_t)width * (bpp/8);
    fb_ptr   = (uint8_t*)fb_phys_addr;

    uint32_t size  = fb_pitch * height;
    uint32_t start = fb_phys_addr & ~0xFFFu;
    uint32_t end   = (fb_phys_addr + size + 0xFFF) & ~0xFFFu;
    for (uint32_t p = start; p < end; p += PAGE_SIZE) paging_map(p, p, PAGE_WRITE);

    return 0;
}

static uint32_t gui_pal_rgb[256];

static int gui_theme = 0;

typedef enum { CONFIRM_NONE = 0, CONFIRM_DELETE_FILE, CONFIRM_KILL_TASK, CONFIRM_REBOOT, CONFIRM_POWEROFF } confirm_kind_t;
static confirm_kind_t confirm_pending = CONFIRM_NONE;
static char           confirm_target_name[64];
static int            confirm_target_pid;

static int term_pid[VGA_TTYS] = { -1, -1, -1 };

#define GUI_THEME_COUNT 3
static const char *gui_theme_names[GUI_THEME_COUNT] = { "Sky", "Sunset", "Forest" };
static const struct { uint8_t r0,g0,b0, r1,g1,b1, r2,g2,b2; } gui_theme_stops[GUI_THEME_COUNT] = {
    { 110,140,210,  55,75,140,  15,18,42 },
    { 235,170,120,  200,95,90,  50,20,45 },
    { 170,215,175,  70,140,95,  15,45,30 },
};

static void build_palette(void) {
    struct { uint8_t r,g,b; } pal[64] = {
        {0,0,0},
        {0,0,42},
        {0,42,0},
        {0,42,42},
        {42,0,0},
        {42,0,42},
        {42,21,0},
        {42,42,42},
        {21,21,21},
        {21,21,63},
        {21,63,21},
        {21,63,63},
        {63,21,21},
        {63,21,63},
        {63,63,21},
        {63,63,63},
        {63,32,0},
        {0,0,32},
        {0,32,32},
        {32,63,0},
        {32,0,48},
        {0,0,20},
        {10,20,42},
        {16,28,50},
        {8,16,40},
        {52,52,56},
        {28,28,32},
        {4,4,4},
        {40,40,44},
        {50,50,55},
        {20,32,56},
        {63,63,0},

        {18,28,58}, {16,25,54}, {14,22,50}, {12,20,46},
        {10,18,43}, {9,17,41},  {8,16,40},  {6,13,35},

        {14,18,26}, {13,16,24}, {12,15,22}, {11,14,20},
        {10,13,19}, {9,11,17},  {8,10,15},  {6,9,14},

        {22,34,58}, {19,30,54}, {16,26,50}, {13,22,45},
        {10,18,40}, {7,14,34},  {5,10,28},  {2,6,20},

        {20,55,63}, {17,50,60}, {14,45,57}, {11,40,54},
        {8,35,50},  {6,30,46},  {4,25,42},  {2,20,38},
    };
    for (int i = 0; i < 64; i++)
        gui_pal_rgb[i] = ((uint32_t)(pal[i].r*255/63) << 16) |
                         ((uint32_t)(pal[i].g*255/63) << 8)  |
                          (uint32_t)(pal[i].b*255/63);

    {
        int th = (gui_theme >= 0 && gui_theme < GUI_THEME_COUNT) ? gui_theme : 0;
        int r0=gui_theme_stops[th].r0, g0=gui_theme_stops[th].g0, b0=gui_theme_stops[th].b0;
        int r1=gui_theme_stops[th].r1, g1=gui_theme_stops[th].g1, b1=gui_theme_stops[th].b1;
        int r2=gui_theme_stops[th].r2, g2=gui_theme_stops[th].g2, b2=gui_theme_stops[th].b2;
        int half = C_GRAD_SKY2_BANDS / 2;
        for (int i = 0; i < C_GRAD_SKY2_BANDS; i++) {
            int r, g, b;
            if (i < half) {
                int t = i, n = half;
                r = r0 + (r1-r0)*t/n;
                g = g0 + (g1-g0)*t/n;
                b = b0 + (b1-b0)*t/n;
            } else {
                int t = i - half, n = C_GRAD_SKY2_BANDS - half;
                r = r1 + (r2-r1)*t/n;
                g = g1 + (g2-g1)*t/n;
                b = b1 + (b2-b1)*t/n;
            }
            gui_pal_rgb[C_GRAD_SKY2+i] = ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
        }
    }
}

static uint8_t *backbuf = 0;
static uint8_t *frontbuf = 0;
static int      front_valid = 0;

static uint8_t *desktop_cache = 0;

static void gui_load_settings(void) {
    if (!fat12_mounted()) return;
    char buf[8];
    int n = fat12_read("SETTINGS.TXT", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    int t = buf[0] - '0';
    if (t >= 0 && t < GUI_THEME_COUNT) gui_theme = t;
}

static void gui_save_settings(void) {
    if (!fat12_mounted()) return;
    char buf[2] = { (char)('0' + gui_theme), '\n' };
    fat12_write("SETTINGS.TXT", buf, 2);
}

void gui_init(void) {
    gui_active = 1;
    if (set_vbe_mode(GUI_WIDTH, GUI_HEIGHT, 32) != 0)
        serial_printf("[gui] no VBE framebuffer found (need qemu -vga std)\r\n");
    gui_load_settings();
    build_palette();
    if (!backbuf) backbuf = (uint8_t*)vmalloc(GUI_WIDTH * GUI_HEIGHT);
    if (!desktop_cache) desktop_cache = (uint8_t*)vmalloc(GUI_WIDTH * GUI_HEIGHT);
    if (!frontbuf) frontbuf = (uint8_t*)vmalloc(GUI_WIDTH * GUI_HEIGHT);
    front_valid = 0;
    mouse_set_bounds(GUI_WIDTH - 1, GUI_HEIGHT - 1);
}

void gui_emergency_text(void) {
    if (!gui_active) return;
    gui_active = 0;
    vbe_write(VBE_IDX_ENABLE, VBE_DISABLED);
    set_mode3h();
    vga_font_restore();
}

void gui_exit(void) {
    if (fb_ptr) {
        uint32_t total = fb_pitch * GUI_HEIGHT;
        for (uint32_t i = 0; i < total; i += 4) *(uint32_t*)(fb_ptr + i) = 0;
    }
    vbe_write(VBE_IDX_ENABLE, VBE_DISABLED);
    set_mode3h();
    vga_font_restore();
    vga_init();
    mouse_set_bounds(79, 24);
}

void gui_pixel(int x, int y, uint8_t c) {
    if (backbuf && (unsigned)x < GUI_WIDTH && (unsigned)y < GUI_HEIGHT)
        backbuf[y * GUI_WIDTH + x] = c;
}

static int row_changed(const uint8_t *a, const uint8_t *b) {
    const uint32_t *pa = (const uint32_t *)a, *pb = (const uint32_t *)b;
    for (int i = 0; i < GUI_WIDTH / 4; i++) if (pa[i] != pb[i]) return 1;
    return 0;
}

static void gui_flip(void) {
    if (!backbuf || !fb_ptr) return;
    for (uint32_t y = 0; y < GUI_HEIGHT; y++) {
        uint8_t *src = backbuf + y*GUI_WIDTH;
        if (frontbuf) {
            uint8_t *seen = frontbuf + y*GUI_WIDTH;
            if (front_valid && !row_changed(src, seen)) continue;
            kmemcpy(seen, src, GUI_WIDTH);
        }
        uint32_t *row = (uint32_t*)(fb_ptr + y*fb_pitch);
        for (uint32_t x = 0; x < GUI_WIDTH; x++) row[x] = gui_pal_rgb[src[x]];
    }
    if (frontbuf) front_valid = 1;
}

void gui_clear(uint8_t c) {
    if (backbuf) kmemset(backbuf, c, GUI_WIDTH * GUI_HEIGHT);
}

void gui_rect_fill(int x, int y, int w, int h, uint8_t c) {
    if (!backbuf) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GUI_WIDTH)  w = GUI_WIDTH - x;
    if (y + h > GUI_HEIGHT) h = GUI_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++)
        kmemset(backbuf + row * GUI_WIDTH + x, c, (size_t)w);
}

void gui_rect(int x, int y, int w, int h, uint8_t c) {
    gui_hline(x,   y,     w, c);
    gui_hline(x,   y+h-1, w, c);
    gui_vline(x,   y,     h, c);
    gui_vline(x+w-1, y,   h, c);
}

void gui_hline(int x, int y, int len, uint8_t c) {
    gui_rect_fill(x, y, len, 1, c);
}
void gui_vline(int x, int y, int len, uint8_t c) {
    gui_rect_fill(x, y, 1, len, c);
}
void gui_line(int x0,int y0,int x1,int y1,uint8_t c){
    int dx=x1-x0,dy=y1-y0,steps;
    if((dx<0?-dx:dx)>(dy<0?-dy:dy)) steps=dx<0?-dx:dx;
    else steps=dy<0?-dy:dy;
    if(!steps){gui_pixel(x0,y0,c);return;}

    int sx=(dx<<8)/steps, sy=(dy<<8)/steps;
    int cx=x0<<8, cy=y0<<8;
    for(int i=0;i<=steps;i++){
        gui_pixel(cx>>8,cy>>8,c); cx+=sx; cy+=sy;
    }
}

void gui_putchar(int x, int y, char c, uint8_t fg, uint8_t bg) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 96) idx = 0;
    const uint8_t *glyph = font8x8[idx];
    if (backbuf && x >= 0 && y >= 0 && x + 8 <= GUI_WIDTH && y + 8 <= GUI_HEIGHT) {
        uint8_t *dst = backbuf + y * GUI_WIDTH + x;
        for (int row = 0; row < 8; row++, dst += GUI_WIDTH) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) dst[col] = (bits & (1 << col)) ? fg : bg;
        }
        return;
    }
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint8_t color = (bits & (0x01 << col)) ? fg : bg;
            gui_pixel(x+col, y+row, color);
        }
    }
}

void gui_puts(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 9; s++; continue; }
        if (cx + 8 > GUI_WIDTH) { cx = x; y += 9; }
        gui_putchar(cx, y, *s++, fg, bg);
        cx += 8;
    }
}

void gui_printf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) {
    char buf[128]; int pos=0;
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    while(*fmt && pos<126){
        if(*fmt!='%'){buf[pos++]=*fmt++;continue;}
        fmt++;
        if(*fmt=='s'){char*s=__builtin_va_arg(ap,char*);while(*s&&pos<126)buf[pos++]=*s++;}
        else if(*fmt=='d'){int d=__builtin_va_arg(ap,int);char t[12];int i=10;t[11]=0;if(!d){buf[pos++]='0';}else{if(d<0){buf[pos++]='-';d=-d;}unsigned u=(unsigned)d;while(u){t[i--]='0'+u%10;u/=10;}while(++i<=10)buf[pos++]=t[i];}
        }else if(*fmt=='u'){unsigned u=__builtin_va_arg(ap,unsigned);char t[12];int i=10;t[11]=0;if(!u){buf[pos++]='0';}else{while(u){t[i--]='0'+u%10;u/=10;}while(++i<=10)buf[pos++]=t[i];}}
        else if(*fmt=='x'){unsigned u=__builtin_va_arg(ap,unsigned);const char*h="0123456789ABCDEF";char t[12];int i=10;t[11]=0;if(!u){buf[pos++]='0';}else{while(u){t[i--]=h[u&0xF];u>>=4;}while(++i<=10)buf[pos++]=t[i];}}
        else if(*fmt=='c'){buf[pos++]=(char)__builtin_va_arg(ap,int);}
        else if(*fmt=='%'){buf[pos++]='%';}
        fmt++;
    }
    buf[pos]=0; __builtin_va_end(ap);
    gui_puts(x, y, buf, fg, bg);
}

static void fill_rounded(int x, int y, int w, int h, uint8_t c) {
    for (int row = 0; row < h; row++) {
        int inset = (row==0||row==h-1) ? 2 : (row==1||row==h-2) ? 1 : 0;
        int rw = w - 2*inset;
        if (rw > 0) gui_hline(x+inset, y+row, rw, c);
    }
}

static void fill_rounded_grad(int x, int y, int w, int h, uint8_t grad_base, int grad_n) {
    for (int row = 0; row < h; row++) {
        int inset = (row==0||row==h-1) ? 2 : (row==1||row==h-2) ? 1 : 0;
        int rw = w - 2*inset;
        int band = (row * grad_n) / (h > 1 ? h : 1);
        if (band >= grad_n) band = grad_n-1;
        if (rw > 0) gui_hline(x+inset, y+row, rw, (uint8_t)(grad_base+band));
    }
}

static void outline_rounded(int x, int y, int w, int h, uint8_t c) {
    gui_hline(x+2, y,     w-4, c);
    gui_hline(x+2, y+h-1, w-4, c);
    gui_vline(x,   y+2,   h-4, c);
    gui_vline(x+w-1, y+2, h-4, c);
    gui_pixel(x+1,   y+1,   c); gui_pixel(x+w-2, y+1,   c);
    gui_pixel(x+1,   y+h-2, c); gui_pixel(x+w-2, y+h-2, c);
}

void gui_window(int x, int y, int w, int h, const char *title) {
    fill_rounded(x+3, y+3, w, h, C_SHADOW);

    fill_rounded(x, y, w, h, C_WIN_BG);

    fill_rounded_grad(x, y, w, 11, C_GRAD_TITLE, 8);

    outline_rounded(x, y, w, h, C_WIN_BORDER);

    gui_puts(x+4, y+2, title, C_LIGHT_CYAN, C_GRAD_TITLE+3);

    gui_rect_fill(x+w-11, y+1, 9, 9, C_RED);
    gui_puts(x+w-9, y+2, "X", C_WHITE, C_RED);

    gui_rect_fill(x+w-22, y+1, 9, 9, C_GRAD_ACCENT+2);
    gui_hline(x+w-20, y+6, 5, C_WHITE);

    gui_line(x+w-6, y+h-2, x+w-2, y+h-6, C_WIN_BORDER);
    gui_line(x+w-5, y+h-2, x+w-2, y+h-5, C_WIN_BORDER);
    gui_line(x+w-4, y+h-2, x+w-2, y+h-4, C_WIN_BORDER);
}

static int point_in(int px, int py, int x, int y, int w, int h) {
    return px>=x && px<x+w && py>=y && py<y+h;
}

static int cur_mx = -1, cur_my = -1, cur_mdown = 0;

static int gui_pressed(int x, int y, int w, int h) {
    return cur_mdown && point_in(cur_mx, cur_my, x, y, w, h);
}

static int gui_hover(int x, int y, int w, int h) {
    return point_in(cur_mx, cur_my, x, y, w, h);
}

void gui_button(int x, int y, int w, int h, const char *label, int pressed) {
    uint8_t bg = pressed ? C_BUTTON : (gui_hover(x, y, w, h) ? (uint8_t)(C_GRAD_ACCENT+2) : C_BUTTON_HI);
    gui_rect_fill(x, y, w, h, bg);
    gui_rect(x, y, w, h, C_WIN_BORDER);
    int tx = x + (w - (int)kstrlen(label)*8) / 2;
    int ty = y + (h - 8) / 2;
    gui_puts(tx, ty, label, C_WHITE, bg);
}

static void icon_glyph(int x, int y, icon_kind_t kind) {
    switch (kind) {
    case ICON_TERM:
        gui_rect_fill(x, y, 20, 14, C_BLACK);
        gui_line(x+3, y+4, x+7, y+7, C_LIGHT_GREEN);
        gui_line(x+3, y+10, x+7, y+7, C_LIGHT_GREEN);
        gui_hline(x+9, y+10, 7, C_LIGHT_GREEN);
        break;
    case ICON_FOLDER:
        gui_rect_fill(x, y+3, 8, 3, C_YELLOW);
        gui_rect_fill(x, y+5, 20, 9, C_YELLOW);
        gui_rect(x, y+5, 20, 9, C_BROWN);
        break;
    case ICON_MONITOR:
        gui_rect_fill(x, y, 20, 10, C_BLACK);
        gui_rect(x, y, 20, 10, C_LIGHT_GREY);
        gui_hline(x+2, y+2, 6, C_LIGHT_GREEN);
        gui_hline(x+2, y+5, 10, C_LIGHT_GREEN);
        gui_rect_fill(x+8, y+10, 4, 2, C_LIGHT_GREY);
        gui_hline(x+5, y+13, 10, C_LIGHT_GREY);
        break;
    case ICON_CALC:
        gui_rect_fill(x, y, 20, 14, C_WHITE);
        gui_rect_fill(x+2, y+2, 16, 4, C_LIGHT_GREEN);
        for (int r=0;r<2;r++)
            for (int c=0;c<4;c++)
                gui_rect_fill(x+2+c*4, y+8+r*4, 3, 3, C_DARK_GREY);
        break;
    case ICON_NOTE:
        gui_rect_fill(x, y, 20, 14, C_WHITE);
        gui_hline(x+3, y+3, 14, C_DARK_GREY);
        gui_hline(x+3, y+7, 14, C_DARK_GREY);
        gui_hline(x+3, y+11, 9, C_DARK_GREY);
        break;
    case ICON_EXIT:
        gui_rect(x+2, y, 12, 14, C_LIGHT_GREY);
        gui_vline(x+6, y+3, 8, C_LIGHT_GREY);
        gui_hline(x+13, y+7, 7, C_LIGHT_RED);
        gui_line(x+17, y+4, x+20, y+7, C_LIGHT_RED);
        gui_line(x+17, y+10, x+20, y+7, C_LIGHT_RED);
        break;
    case ICON_PKG:
        gui_rect_fill(x+1, y+3, 18, 11, C_BROWN);
        gui_line(x+1, y+3, x+10, y, C_LIGHT_GREY);
        gui_line(x+19, y+3, x+10, y, C_LIGHT_GREY);
        gui_hline(x+8, y+7, 4, C_YELLOW);
        gui_vline(x+9, y+5, 4, C_YELLOW);
        break;
    case ICON_BROWSER:

        gui_hline(x+4, y+1, 12, C_LIGHT_BLUE);
        gui_hline(x+2, y+3, 16, C_LIGHT_BLUE);
        gui_hline(x+1, y+5, 18, C_LIGHT_BLUE);
        gui_hline(x+1, y+7, 18, C_LIGHT_BLUE);
        gui_hline(x+1, y+9, 18, C_LIGHT_BLUE);
        gui_hline(x+2, y+11, 16, C_LIGHT_BLUE);
        gui_hline(x+4, y+13, 12, C_LIGHT_BLUE);
        gui_vline(x+10, y+1, 12, C_NAVY);
        gui_hline(x+2, y+7, 16, C_NAVY);
        break;
    case ICON_PAINT:
        gui_rect_fill(x, y+2, 20, 11, C_WHITE);
        gui_rect(x, y+2, 20, 11, C_DARK_GREY);
        gui_rect_fill(x+2,  y+4, 3, 3, C_RED);
        gui_rect_fill(x+7,  y+4, 3, 3, C_LIGHT_GREEN);
        gui_rect_fill(x+12, y+4, 3, 3, C_LIGHT_BLUE);
        gui_rect_fill(x+2,  y+9, 3, 3, C_YELLOW);
        gui_rect_fill(x+7,  y+9, 3, 3, C_PURPLE);
        gui_rect_fill(x+12, y+9, 3, 3, C_BLACK);
        break;
    case ICON_SNAKE: {
        int sx[6] = {1,4,7,7,10,13}, sy[6] = {6,6,6,3,3,3};
        for (int i=0;i<6;i++) {
            uint8_t c = (i==5) ? C_LIGHT_RED : C_LIGHT_GREEN;
            gui_rect_fill(x+sx[i], y+sy[i], 3, 3, c);
        }
        break;
    }
    case ICON_PIANO:
        gui_rect_fill(x, y+1, 20, 12, C_WHITE);
        gui_rect(x, y+1, 20, 12, C_DARK_GREY);
        gui_rect_fill(x+3,  y+1, 3, 8, C_BLACK);
        gui_rect_fill(x+9,  y+1, 3, 8, C_BLACK);
        gui_vline(x+6,  y+1, 12, C_DARK_GREY);
        gui_vline(x+13, y+1, 12, C_DARK_GREY);
        break;
    case ICON_2048:
        gui_rect_fill(x,   y+1, 9, 6,  C_ORANGE);
        gui_rect_fill(x+11,y+1, 9, 6,  C_YELLOW);
        gui_rect_fill(x,   y+8, 9, 6,  C_LIGHT_BLUE);
        gui_rect_fill(x+11,y+8, 9, 6,  C_LIGHT_GREEN);
        break;
    case ICON_MINE:
        gui_rect_fill(x, y+2, 20, 11, C_LIGHT_GREY);
        gui_rect_fill(x+7, y+4, 6, 6, C_BLACK);
        gui_rect_fill(x+2, y+5, 3, 3, C_BLACK);
        gui_rect_fill(x+15, y+5, 3, 3, C_BLACK);
        gui_pixel(x+9, y, C_BLACK);
        gui_pixel(x+10, y+1, C_RED);
        break;
    case ICON_SETTINGS:

        gui_rect_fill(x+6, y+3, 8, 8, C_LIGHT_GREY);
        gui_rect_fill(x+8, y+5, 4, 4, C_DARK_GREY);
        gui_rect_fill(x+9, y,   2, 3, C_LIGHT_GREY);
        gui_rect_fill(x+9, y+11,2, 3, C_LIGHT_GREY);
        gui_rect_fill(x,   y+5, 3, 2, C_LIGHT_GREY);
        gui_rect_fill(x+17,y+5, 3, 2, C_LIGHT_GREY);
        gui_pixel(x+3, y+2, C_LIGHT_GREY);
        gui_pixel(x+16,y+2, C_LIGHT_GREY);
        gui_pixel(x+3, y+11,C_LIGHT_GREY);
        gui_pixel(x+16,y+11,C_LIGHT_GREY);
        break;
    }
}

void gui_icon(int x, int y, const char *label, uint8_t icon_color, icon_kind_t kind) {
    fill_rounded(x+1, y+1, 30, 22, C_SHADOW);
    fill_rounded(x, y, 30, 22, icon_color);
    outline_rounded(x, y, 30, 22, gui_hover(x, y, 30, 22) ? C_WHITE : C_WIN_BORDER);

    icon_glyph(x+5, y+4, kind);

    int lx = x + (30 - (int)kstrlen(label)*8)/2;
    if (lx < x) lx = x;
    gui_puts(lx, y+23, label, C_WHITE, C_DESKTOP);
}

#define CUR_W 8
#define CUR_H 12

static const uint8_t cursor_shape[CUR_H][CUR_W] = {
    {1,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0},
    {1,2,2,1,0,0,0,0},
    {1,2,2,2,1,0,0,0},
    {1,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,0},
    {1,2,2,2,1,1,1,0},
    {1,2,1,2,1,0,0,0},
    {1,1,0,1,2,1,0,0},
    {0,0,0,0,1,2,1,0},
    {0,0,0,0,0,1,1,0},
};

void gui_draw_cursor(int x, int y) {
    for (int row=0;row<CUR_H;row++)
        for (int col=0;col<CUR_W;col++) {
            uint8_t s=cursor_shape[row][col];
            if (s==1) gui_pixel(x+col, y+row, C_WHITE);
            else if(s==2) gui_pixel(x+col, y+row, C_BLACK);
        }
}

static void gui_textfield_draw(int x, int y, int w, int h, const char *buf, int editing) {
    gui_rect_fill(x, y, w, h, C_WHITE);
    gui_rect(x, y, w, h, editing ? C_NAVY : C_LIGHT_GREY);
    gui_puts(x+2, y+2, buf, C_BLACK, C_WHITE);
    if (editing && (timer_ticks()/50) & 1)
        gui_rect_fill(x+2+(int)kstrlen(buf)*8, y+1, 6, 9, C_LIGHT_GREY);
}

static void gui_textfield_key(char *buf, int maxlen, char key) {
    if (key == '\b') {
        int n = (int)kstrlen(buf);
        if (n > 0) buf[n-1] = 0;
    } else if (key >= 32 && (uint8_t)key < 127) {
        int n = (int)kstrlen(buf);
        if (n < maxlen-1) { buf[n] = key; buf[n+1] = 0; }
    }
}

static uint32_t gui_rng_state = 0;

static uint32_t gui_rand(void) {
    if (!gui_rng_state) gui_rng_state = timer_ticks() ^ 0x9E3779B9u;
    gui_rng_state = gui_rng_state * 1103515245u + 12345u;
    return gui_rng_state;
}

static uint32_t gui_beep_off_at = 0;

static void gui_beep(uint32_t freq_hz, uint32_t dur_ticks) {
    speaker_on(freq_hz);
    gui_beep_off_at = timer_ticks() + dur_ticks;
}

#define ICON_START_Y 16
#define ICON_SLOT    38

typedef enum {
    WIN_TERMINAL=0, WIN_TERMINAL2=1, WIN_TERMINAL3=2, WIN_FILES=3, WIN_SYSMON=4,
    WIN_CALC=5, WIN_EDITOR=6, WIN_PKG=7, WIN_BROWSER=8, WIN_WRITE=9, WIN_PAINT=10, WIN_SNAKE=11,
    WIN_PIANO=12, WIN_2048=13, WIN_MINE=14, WIN_SETTINGS=15, WIN_COUNT=16
} wintype_t;

typedef struct { int active, x, y, w, h, minimized, min_w, min_h, maxed, rx, ry, rw, rh; } winrec_t;

static winrec_t wins[WIN_COUNT];
static int      zorder[WIN_COUNT];
static int      zcount = 0;

static const char *win_title(int t) {
    switch (t) {
        case WIN_TERMINAL:  return "Terminal";
        case WIN_TERMINAL2: return "Terminal 2";
        case WIN_TERMINAL3: return "Terminal 3";
        case WIN_FILES:    return "Files";
        case WIN_SYSMON:   return "System Monitor";
        case WIN_CALC:     return "Calculator";
        case WIN_EDITOR:   return "Notepad";
        case WIN_PKG:      return "Packages";
        case WIN_BROWSER:  return "Web";
        case WIN_WRITE:    return "KumWrite";
        case WIN_PAINT:    return "Paint";
        case WIN_SNAKE:    return "Snake";
        case WIN_PIANO:    return "Piano";
        case WIN_2048:     return "2048";
        case WIN_MINE:     return "Minesweeper";
        case WIN_SETTINGS: return "Settings";
    }
    return "";
}

static void wm_remove_z(int t) {
    int idx = -1;
    for (int i=0;i<zcount;i++) if (zorder[i]==t) { idx=i; break; }
    if (idx < 0) return;
    for (int i=idx;i<zcount-1;i++) zorder[i]=zorder[i+1];
    zcount--;
}

static void wm_push_front(int t) {
    wm_remove_z(t);
    zorder[zcount++] = t;
}

static int wm_topmost(void) {
    for (int i=zcount-1;i>=0;i--)
        if (!wins[zorder[i]].minimized) return zorder[i];
    return -1;
}

static void wm_cycle_focus(void) {
    if (zcount < 2) return;
    int top = wm_topmost();
    if (top < 0) return;
    int topidx = -1;
    for (int i=0;i<zcount;i++) if (zorder[i]==top) { topidx=i; break; }
    for (int step=1; step<zcount; step++) {
        int idx = (topidx - step + zcount) % zcount;
        int cand = zorder[idx];
        if (wins[cand].active && !wins[cand].minimized) { wm_push_front(cand); return; }
    }
}

static void files_refresh(void);
static int  term_alive(int tty);
static void term_open_session(int tty);
static void term_close_session(int tty);
static void pkg_refresh(void);

static void wm_open(int t, int defx, int defy, int defw, int defh) {
    if (t >= WIN_TERMINAL && t <= WIN_TERMINAL3 && !term_alive(t - WIN_TERMINAL))
        term_open_session(t - WIN_TERMINAL);
    if (!wins[t].active) {
        wins[t].active = 1;
        wins[t].x = defx; wins[t].y = defy; wins[t].w = defw; wins[t].h = defh;
        wins[t].min_w = defw; wins[t].min_h = defh;
        wins[t].minimized = 0;
        if (t == WIN_FILES) files_refresh();
        if (t == WIN_PKG) pkg_refresh();
    }
    wins[t].minimized = 0;
    wm_push_front(t);
}

static void wm_close(int t) {
    if (t >= WIN_TERMINAL && t <= WIN_TERMINAL3) term_close_session(t - WIN_TERMINAL);
    wins[t].active = 0;
    wm_remove_z(t);
}

static void wm_close_all(void) {
    for (int t=0;t<WIN_COUNT;t++)
        if (wins[t].active) wm_close(t);
}

static void wm_toggle_maximize(int t) {
    winrec_t *w = &wins[t];
    if (w->maxed) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maxed = 0;
    } else {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 10; w->w = GUI_WIDTH; w->h = GUI_HEIGHT-10;
        w->maxed = 1;
    }
}

static void wm_cascade(void) {
    int i = 0;
    for (int z=0; z<zcount; z++) {
        winrec_t *w = &wins[zorder[z]];
        if (!w->active || w->minimized) continue;
        int nx = 44 + i*28, ny = 14 + i*22;
        if (nx + w->w > GUI_WIDTH)  nx = 44;
        if (ny + w->h > GUI_HEIGHT) ny = 14;
        w->x = nx; w->y = ny;
        i++;
    }
}

static void wm_tile(void) {
    int list[WIN_COUNT], n = 0;
    for (int z=0; z<zcount; z++) {
        int t = zorder[z];
        if (wins[t].active && !wins[t].minimized) list[n++] = t;
    }
    if (n == 0) return;
    int cols = 1; while (cols*cols < n) cols++;
    int rows = (n + cols - 1) / cols;
    int areaY = 10, areaH = GUI_HEIGHT - areaY;
    int cellW = GUI_WIDTH / cols, cellH = areaH / rows;
    for (int i=0;i<n;i++) {
        int r = i / cols, c = i % cols;
        winrec_t *w = &wins[list[i]];
        w->x = c*cellW + 2;
        w->y = areaY + r*cellH + 2;
        w->w = cellW - 4; if (w->w < w->min_w) w->w = w->min_w;
        w->h = cellH - 4; if (w->h < w->min_h) w->h = w->min_h;
    }
}

#define TB_TAB_MAX WIN_COUNT
static int tb_tab_type[TB_TAB_MAX];
static int tb_tab_x0[TB_TAB_MAX];
static int tb_tab_x1[TB_TAB_MAX];
static int tb_tab_count = 0;
static int tb_cascade_x0, tb_cascade_x1;
static int tb_tile_x0, tb_tile_x1;

void gui_draw_taskbar(void) {
    for (int y=0;y<9;y++) {
        int band = (y*8)/9; if (band>7) band=7;
        gui_hline(0, y, GUI_WIDTH, (uint8_t)(C_GRAD_TASKBAR+band));
    }
    gui_hline(0, 9, GUI_WIDTH, (uint8_t)(C_GRAD_ACCENT+5));

    fill_rounded(1, 1, 46, 8, (uint8_t)(C_GRAD_ACCENT+2));
    gui_puts(4, 1, "KumOS", C_WHITE, (uint8_t)(C_GRAD_ACCENT+2));

    tb_tab_count = 0;
    int tx = 51;
    int top = wm_topmost();
    for (int i=0;i<zcount;i++) {
        int t = zorder[i];
        if (!wins[t].active) continue;
        int tw = (int)kstrlen(win_title(t))*8 + 6;
        int active = (t==top && !wins[t].minimized);
        uint8_t bg = active ? (uint8_t)(C_GRAD_ACCENT+3) : (uint8_t)(C_GRAD_TASKBAR+1);
        uint8_t fg = wins[t].minimized ? C_LIGHT_GREY : C_WHITE;
        fill_rounded(tx, 0, tw, 9, bg);
        gui_puts(tx+3, 1, win_title(t), fg, bg);
        if (active) gui_hline(tx+2, 8, tw-4, C_WHITE);
        tb_tab_type[tb_tab_count]=t; tb_tab_x0[tb_tab_count]=tx; tb_tab_x1[tb_tab_count]=tx+tw;
        tb_tab_count++;
        tx += tw + 3;
    }

    tb_cascade_x0 = GUI_WIDTH - 178; tb_cascade_x1 = tb_cascade_x0 + 11;
    fill_rounded(tb_cascade_x0, 0, 11, 9, (uint8_t)(C_GRAD_TASKBAR+2));
    gui_putchar(tb_cascade_x0+2, 1, 'C', C_WHITE, (uint8_t)(C_GRAD_TASKBAR+2));

    tb_tile_x0 = tb_cascade_x1 + 2; tb_tile_x1 = tb_tile_x0 + 11;
    fill_rounded(tb_tile_x0, 0, 11, 9, (uint8_t)(C_GRAD_TASKBAR+2));
    gui_putchar(tb_tile_x0+2, 1, 'T', C_WHITE, (uint8_t)(C_GRAD_TASKBAR+2));

    rtc_time_t t = rtc_read();
    char timebuf[10];

    timebuf[0]='0'+t.hour/10;   timebuf[1]='0'+t.hour%10;
    timebuf[2]=':';
    timebuf[3]='0'+t.minute/10; timebuf[4]='0'+t.minute%10;
    timebuf[5]=':';
    timebuf[6]='0'+t.second/10; timebuf[7]='0'+t.second%10;
    timebuf[8]=0;
    gui_puts(GUI_WIDTH-67, 1, timebuf, C_YELLOW, C_GRAD_TASKBAR);

    char ubuf[20];
    uint32_t up=timer_seconds();
    int ui=17; ubuf[18]='s'; ubuf[19]=0;
    if(!up){ubuf[ui--]='0';}else{uint32_t u=up;while(u){ubuf[ui--]='0'+u%10;u/=10;}}
    ubuf[ui--]='p'; ubuf[ui--]='u';
    gui_puts(GUI_WIDTH-130, 1, ubuf+ui+1, C_LIGHT_GREY, C_GRAD_TASKBAR);
}

static int taskbar_hit(int mx, int my) {
    if (my >= 10) return -1;
    for (int i=0;i<tb_tab_count;i++)
        if (mx>=tb_tab_x0[i] && mx<tb_tab_x1[i]) return tb_tab_type[i];
    return -2;
}

void gui_draw_desktop(void) {
    int dh = GUI_HEIGHT - 10;
    for (int y=10;y<GUI_HEIGHT;y++) {
        int band = ((y-10) * C_GRAD_SKY2_BANDS) / dh;
        if (band > C_GRAD_SKY2_BANDS-1) band = C_GRAD_SKY2_BANDS-1;
        gui_hline(0, y, GUI_WIDTH, (uint8_t)(C_GRAD_SKY2+band));
    }

    gui_icon(8, ICON_START_Y + 0*ICON_SLOT, "Term",  C_TEAL,       ICON_TERM);
    gui_icon(8, ICON_START_Y + 1*ICON_SLOT, "Files", C_NAVY,       ICON_FOLDER);
    gui_icon(8, ICON_START_Y + 2*ICON_SLOT, "Sys",   C_PURPLE,     ICON_MONITOR);
    gui_icon(8, ICON_START_Y + 3*ICON_SLOT, "Calc",  C_BROWN,      ICON_CALC);
    gui_icon(8, ICON_START_Y + 4*ICON_SLOT, "Note",  C_LIGHT_BLUE, ICON_NOTE);
    gui_icon(8, ICON_START_Y + 5*ICON_SLOT, "Pkgs",  C_TEAL,       ICON_PKG);
    gui_icon(8, ICON_START_Y + 6*ICON_SLOT, "Web",   C_LIGHT_BLUE, ICON_BROWSER);
    gui_icon(8, ICON_START_Y + 7*ICON_SLOT, "Write", C_PINK,       ICON_NOTE);
    gui_icon(8, ICON_START_Y + 8*ICON_SLOT, "Paint", C_ORANGE,     ICON_PAINT);
    gui_icon(8, ICON_START_Y + 9*ICON_SLOT, "Snake", C_GREEN,      ICON_SNAKE);
    gui_icon(8, ICON_START_Y + 10*ICON_SLOT, "Piano", C_PURPLE,    ICON_PIANO);
    gui_icon(8, ICON_START_Y + 11*ICON_SLOT, "2048",  C_ORANGE,    ICON_2048);
    gui_icon(8, ICON_START_Y + 12*ICON_SLOT, "Mines", C_LIGHT_GREY,ICON_MINE);
    gui_icon(8, ICON_START_Y + 13*ICON_SLOT, "Set",   C_DARK_GREY, ICON_SETTINGS);
    gui_icon(8, ICON_START_Y + 14*ICON_SLOT, "Exit",  C_DARK_GREY, ICON_EXIT);

    gui_printf(52, 18, C_LIGHT_GREY, C_DESKTOP,
               "Click icons to open apps");
    gui_printf(52, 28, C_DARK_GREY,  C_DESKTOP,
               "Drag bar to move, Esc to close");
}

static void gui_apply_theme(void) {
    build_palette();
    front_valid = 0;
    gui_draw_desktop();
    kmemcpy(desktop_cache, backbuf, GUI_WIDTH * GUI_HEIGHT);
}

#define TERM_W  654
#define TERM_H  248

static void wm_open_new_terminal(void) {
    for (int i = 0; i < VGA_TTYS; i++) {
        if (!wins[WIN_TERMINAL + i].active) {
            wm_open(WIN_TERMINAL + i, 40 + i*24, 20 + i*24, TERM_W, TERM_H);
            return;
        }
    }
    wm_open(WIN_TERMINAL, 40, 20, TERM_W, TERM_H);
}

static int term_alive(int tty) {
    if (term_pid[tty] < 0) return 0;
    task_t *t = sched_get_task(term_pid[tty]);
    return t && t->state != TASK_DEAD && t->state != TASK_ZOMBIE;
}

static void term_open_session(int tty) {
    vga_tty_reset(tty);
    const char *argv[1] = { "kush" };
    term_pid[tty] = elf_run("KUSH.ELF", 1, argv, tty);
}

static void term_close_session(int tty) {
    for (int i = 2; i < sched_task_count(); i++) {
        task_t *t = sched_task_at(i);
        if (t && t->pid && t->tty == tty && t->state != TASK_DEAD && t->state != TASK_ZOMBIE)
            signal_send(t->pid, SIGKILL);
    }
    term_pid[tty] = -1;
}

static int focused_tty(void) {
    int top = wm_topmost();
    return (top >= WIN_TERMINAL && top <= WIN_TERMINAL3) ? top - WIN_TERMINAL : -1;
}

int gui_ring3_input_focused(void) {
    if (!gui_active) return 1;
    return focused_tty() == sched_current()->tty;
}

static void render_terminal(winrec_t *w, int tty) {
    gui_window(w->x, w->y, w->w, w->h, win_title(WIN_TERMINAL + tty));
    gui_rect_fill(w->x+1, w->y+12, w->w-2, w->h-13, C_BLACK);
    int tx = w->x+4, ty = w->y+13;

    if (!term_alive(tty)) {
        gui_puts(tx, ty+2, "Session ended. Press any key for a new shell.", C_LIGHT_GREY, C_BLACK);
        return;
    }

    const uint16_t *vb = vga_tty_buffer(tty);
    int rows = (w->h - 14) / 9; if (rows > VGA_HEIGHT) rows = VGA_HEIGHT;
    int cols = (w->w - 6) / 8;  if (cols > VGA_WIDTH)  cols = VGA_WIDTH;
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            uint16_t e = vb[row*VGA_WIDTH + col];
            uint8_t ch = (uint8_t)(e & 0xFF);
            uint8_t fg = (uint8_t)(e >> 8) & 0x0F;
            uint8_t bg = (uint8_t)(e >> 12) & 0x0F;
            if (ch != ' ' || bg != C_BLACK) gui_putchar(tx+col*8, ty+row*9, (char)ch, fg, bg);
        }
    }

    int cr, cc;
    if (vga_tty_cursor(tty, &cr, &cc) && cr < rows && cc < cols && ((timer_ticks()/50)&1))
        gui_rect_fill(tx+cc*8, ty+cr*9, 7, 8, C_LIGHT_GREY);
}

static void term_run_command(const char *cmd) {
    int tty = focused_tty();
    for (int i = 0; i < VGA_TTYS && tty < 0; i++)
        if (wins[WIN_TERMINAL + i].active) tty = i;
    if (tty < 0) tty = 0;
    wm_open(WIN_TERMINAL + tty, 40 + tty*24, 20 + tty*24, TERM_W, TERM_H);
    keyboard_inject(cmd);
    keyboard_inject("\n");
}

#define FILES_MAX    40
#define FILES_ROW_H   9

static fat12_entry_t files_entries[FILES_MAX];
static int  files_count = 0;
static int  files_selected = -1;
static int  files_preview = 0;
static char files_previewbuf[256];
static int  files_renaming = 0;
static char files_renamebuf[13];

static void files_refresh(void) {
    files_count = fat12_mounted() ? fat12_list(files_entries, FILES_MAX) : 0;
    if (files_count < 0) files_count = 0;
    files_selected = -1;
    files_preview = 0;
    files_renaming = 0;
}

static void files_do_rename(void) {
    if (files_selected < 0 || !files_renamebuf[0]) { files_renaming = 0; return; }
    if (kstrcmp(files_renamebuf, files_entries[files_selected].name) == 0) {
        files_renaming = 0;
        return;
    }
    fat12_rename(files_entries[files_selected].name, files_renamebuf);
    files_renaming = 0;
    files_refresh();
}

static void render_files(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Files");
    gui_rect_fill(w->x+1, w->y+12, w->w-2, w->h-13, C_BLACK);

    int cx = w->x+3, cy = w->y+13;

    if (!files_preview) {
        if (!fat12_mounted()) {
            gui_puts(cx, cy, "No disk mounted", C_LIGHT_RED, C_BLACK);
        } else if (files_count <= 0) {
            gui_puts(cx, cy, "(empty)", C_LIGHT_GREY, C_BLACK);
        } else {
            int maxrows = (w->h - 13 - 10) / FILES_ROW_H;
            for (int i=0;i<files_count && i<maxrows;i++) {
                int ry = cy + i*FILES_ROW_H - 1;
                uint8_t fg = (i==files_selected) ? C_BLACK : C_LIGHT_GREEN;
                uint8_t bg = (i==files_selected) ? C_LIGHT_GREEN : C_BLACK;
                if (i==files_selected) gui_rect_fill(w->x+1, ry, w->w-2, FILES_ROW_H, bg);
                gui_puts(cx, cy+i*FILES_ROW_H, files_entries[i].name, fg, bg);
            }
            gui_puts(cx, w->y+w->h-9, "Click a file to view", C_DARK_GREY, C_BLACK);
        }

        gui_button(w->x+w->w-64, w->y+w->h-10, 62, 9, "Refresh",
                   gui_pressed(w->x+w->w-64, w->y+w->h-10, 62, 9));
    } else {
        if (files_renaming) {
            gui_textfield_draw(cx, cy, 100, 12, files_renamebuf, 1);
        } else {
            gui_printf(cx, cy, C_YELLOW, C_BLACK, "%s", files_entries[files_selected].name);
        }
        gui_puts(cx, cy+14, files_previewbuf, C_LIGHT_GREEN, C_BLACK);
        if (files_renaming) {
            gui_button(w->x+w->w-78, w->y+w->h-14, 34, 11, "OK",
                       gui_pressed(w->x+w->w-78, w->y+w->h-14, 34, 11));
            gui_button(w->x+w->w-40, w->y+w->h-14, 34, 11, "Cxl",
                       gui_pressed(w->x+w->w-40, w->y+w->h-14, 34, 11));
        } else {
            gui_button(w->x+w->w-116, w->y+w->h-14, 34, 11, "Ren",
                       gui_pressed(w->x+w->w-116, w->y+w->h-14, 34, 11));
            gui_button(w->x+w->w-78, w->y+w->h-14, 34, 11, "Del",
                       gui_pressed(w->x+w->w-78, w->y+w->h-14, 34, 11));
            gui_button(w->x+w->w-40, w->y+w->h-14, 34, 11, "Back",
                       gui_pressed(w->x+w->w-40, w->y+w->h-14, 34, 11));
        }
    }
}

static void files_handle_click(winrec_t *w, int mx, int my) {
    int cy = w->y+13;

    if (!files_preview) {
        if (point_in(mx, my, w->x+w->w-64, w->y+w->h-10, 62, 9)) { files_refresh(); return; }
        if (files_count <= 0) return;
        int maxrows = (w->h - 13 - 10) / FILES_ROW_H;
        for (int i=0;i<files_count && i<maxrows;i++) {
            int ry = cy + i*FILES_ROW_H - 1;
            if (point_in(mx, my, w->x+1, ry, w->w-2, FILES_ROW_H)) {
                files_selected = i;
                int n = fat12_read(files_entries[i].name, files_previewbuf, sizeof(files_previewbuf)-1);
                if (n < 0) n = 0;
                files_previewbuf[n] = 0;
                files_preview = 1;
                return;
            }
        }
    } else if (files_renaming) {
        if (point_in(mx, my, w->x+w->w-78, w->y+w->h-14, 34, 11)) {
            files_do_rename();
        } else if (point_in(mx, my, w->x+w->w-40, w->y+w->h-14, 34, 11)) {
            files_renaming = 0;
        }
    } else {
        if (point_in(mx, my, w->x+w->w-116, w->y+w->h-14, 34, 11)) {
            kstrcpy(files_renamebuf, files_entries[files_selected].name);
            files_renaming = 1;
        } else if (point_in(mx, my, w->x+w->w-78, w->y+w->h-14, 34, 11)) {
            kstrcpy(confirm_target_name, files_entries[files_selected].name);
            confirm_pending = CONFIRM_DELETE_FILE;
        } else if (point_in(mx, my, w->x+w->w-40, w->y+w->h-14, 34, 11)) {
            files_preview = 0;
        }
    }
}

static void render_sysmon(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "System Monitor");
    gui_rect_fill(w->x+1, w->y+12, w->w-2, w->h-13, C_BLACK);

    int cx = w->x+3, cy = w->y+14;

    uint32_t eax,ebx,ecx,edx; char vendor[13];
    __asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(0));
    kmemcpy(vendor+0,&ebx,4); kmemcpy(vendor+4,&edx,4); kmemcpy(vendor+8,&ecx,4);
    vendor[12]=0;

    gui_printf(cx,cy, C_YELLOW,C_BLACK, "CPU: %s", vendor);  cy += 9;
    gui_printf(cx,cy, C_WHITE, C_BLACK, "Uptime: %ds", (int)timer_seconds()); cy += 9;
    gui_printf(cx,cy, C_WHITE, C_BLACK, "Heap: %uK/%uK", kmalloc_used()/1024, (kmalloc_used()+kmalloc_free())/1024); cy += 9;
    gui_printf(cx,cy, C_WHITE, C_BLACK, "Frames: %u/%u", pmm_used(), pmm_total()); cy += 9;
    gui_printf(cx,cy, C_LIGHT_CYAN,C_BLACK, "Tasks: %d (click to kill)", sched_task_count()); cy += 9;

    if (net_ready()) {
        uint32_t ip = net_get_ip();
        uint32_t txp, rxp; net_get_stats(&txp, &rxp);
        gui_printf(cx,cy, C_LIGHT_GREEN,C_BLACK, "Net: %d.%d.%d.%d",
                   (int)(ip>>24)&0xFF, (int)(ip>>16)&0xFF, (int)(ip>>8)&0xFF, (int)ip&0xFF); cy += 9;
        gui_printf(cx,cy, C_WHITE,C_BLACK, "TX:%u RX:%u pkts", txp, rxp); cy += 9;
    } else {
        gui_printf(cx,cy, C_LIGHT_RED,C_BLACK, "Net: no NIC"); cy += 9;
    }

    int maxrows = (w->y+w->h-9-cy) / 9;
    int shown = 0;
    int cur = sched_current_index();
    for (int i=0;i<sched_task_count() && shown<maxrows;i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->state == TASK_DEAD || t->state == TASK_ZOMBIE) continue;
        uint8_t fg = (i==cur) ? C_LIGHT_GREEN : C_LIGHT_GREY;
        gui_printf(cx,cy,fg,C_BLACK,"%d %s", t->pid, t->name);
        cy += 9; shown++;
    }
}

static void sysmon_handle_click(winrec_t *w, int mx, int my) {
    int cy = w->y + 14 + 9*5;
    cy += net_ready() ? 9*2 : 9;

    int maxrows = (w->y+w->h-9-cy) / 9;
    int shown = 0;
    int cur = sched_current_index();
    for (int i=0;i<sched_task_count() && shown<maxrows;i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->state == TASK_DEAD || t->state == TASK_ZOMBIE) continue;
        if (point_in(mx, my, w->x+1, cy-1, w->w-2, 9)) {
            if (t->pid != 0 && i != cur) {
                confirm_target_pid = t->pid;
                confirm_pending = CONFIRM_KILL_TASK;
            }
            return;
        }
        cy += 9; shown++;
    }
}

static int  calc_acc = 0;
static int  calc_entry = 0;
static char calc_op = 0;
static int  calc_entry_active = 0;
static int  calc_error = 0;

static const char *calc_labels[4][4] = {
    {"7","8","9","/"},
    {"4","5","6","*"},
    {"1","2","3","-"},
    {"0","C","=","+"},
};

#define CALC_BW  30
#define CALC_BH  22
#define CALC_GAP  3

static void calc_grid_origin(winrec_t *w, int *gx, int *gy) {
    *gx = w->x+4; *gy = w->y+34;
}

static int calc_apply(int a, char op, int b) {
    switch (op) {
        case '+': return a+b;
        case '-': return a-b;
        case '*': return a*b;
        case '/': if (!b) { calc_error = 1; return 0; } return a/b;
    }
    return b;
}

static void calc_press(const char *label) {
    char c = label[0];
    if (c>='0' && c<='9') {
        if (!calc_entry_active) { calc_entry = 0; calc_entry_active = 1; }
        if (calc_entry < 100000000) calc_entry = calc_entry*10 + (c-'0');
    } else if (c=='C') {
        calc_acc=0; calc_entry=0; calc_op=0; calc_entry_active=0; calc_error=0;
    } else if (c=='=') {
        if (calc_op) { calc_acc = calc_apply(calc_acc, calc_op, calc_entry); calc_op = 0; }
        else calc_acc = calc_entry;
        calc_entry_active = 0;
    } else {
        if (calc_op && calc_entry_active) calc_acc = calc_apply(calc_acc, calc_op, calc_entry);
        else if (!calc_op && calc_entry_active) calc_acc = calc_entry;
        calc_op = c;
        calc_entry_active = 0;
        calc_entry = 0;
    }
}

static void render_calc(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Calculator");
    gui_rect_fill(w->x+1, w->y+12, w->w-2, w->h-13, C_WIN_BG);

    gui_rect_fill(w->x+4, w->y+16, w->w-8, 14, C_BLACK);
    char dispbuf[16];
    if (calc_error) {
        kstrcpy(dispbuf, "Error");
    } else {
        int dv = calc_entry_active ? calc_entry : calc_acc;
        int pos = 0;
        if (dv < 0) dispbuf[pos++] = '-';
        char tmp[12];
        kitoa((uint32_t)(dv<0 ? -dv : dv), tmp, 10);
        kstrcpy(dispbuf+pos, tmp);
    }
    gui_puts(w->x+6, w->y+18, dispbuf, C_LIGHT_GREEN, C_BLACK);

    int gx, gy;
    calc_grid_origin(w, &gx, &gy);
    for (int r=0;r<4;r++)
        for (int c2=0;c2<4;c2++)
            gui_button(gx+c2*(CALC_BW+CALC_GAP), gy+r*(CALC_BH+CALC_GAP),
                       CALC_BW, CALC_BH, calc_labels[r][c2],
                       gui_pressed(gx+c2*(CALC_BW+CALC_GAP), gy+r*(CALC_BH+CALC_GAP), CALC_BW, CALC_BH));
}

static void calc_handle_click(winrec_t *w, int mx, int my) {
    int gx, gy;
    calc_grid_origin(w, &gx, &gy);
    for (int r=0;r<4;r++)
        for (int c2=0;c2<4;c2++) {
            int bx = gx+c2*(CALC_BW+CALC_GAP), by = gy+r*(CALC_BH+CALC_GAP);
            if (point_in(mx, my, bx, by, CALC_BW, CALC_BH)) { calc_press(calc_labels[r][c2]); return; }
        }
}

#define EDIT_ROWS 20
#define EDIT_COLS 50

static char editor_lines[EDIT_ROWS][EDIT_COLS+1];
static int  editor_row = 0, editor_col = 0;
static char editor_status[20] = "";

static void editor_init_buf(void) {
    for (int i=0;i<EDIT_ROWS;i++) editor_lines[i][0] = 0;
    editor_row = 0; editor_col = 0;
}

static void render_editor(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Notepad");
    gui_rect_fill(w->x+1, w->y+12, w->w-2, w->h-13, C_WHITE);

    int tx = w->x+3, ty = w->y+14;
    for (int i=0;i<EDIT_ROWS;i++)
        gui_puts(tx, ty+i*9, editor_lines[i], C_BLACK, C_WHITE);

    if ((timer_ticks()/50) & 1)
        gui_rect_fill(tx+editor_col*8, ty+editor_row*9, 7, 8, C_LIGHT_GREY);

    gui_button(w->x+4,  w->y+w->h-16, 40, 12, "Save", gui_pressed(w->x+4,  w->y+w->h-16, 40, 12));
    gui_button(w->x+48, w->y+w->h-16, 40, 12, "Load", gui_pressed(w->x+48, w->y+w->h-16, 40, 12));
    gui_puts(w->x+94, w->y+w->h-14, editor_status, C_DARK_GREY, C_WIN_BG);
}

static void editor_save(void) {
    char buf[EDIT_ROWS*(EDIT_COLS+1)]; int n=0;
    for (int i=0;i<=editor_row;i++) {
        const char *p = editor_lines[i];
        while (*p) buf[n++] = *p++;
        if (i < editor_row) buf[n++] = '\n';
    }
    if (!fat12_mounted()) kstrcpy(editor_status, "No disk");
    else if (fat12_write("NOTES.TXT", buf, (uint32_t)n) == 0) kstrcpy(editor_status, "Saved!");
    else kstrcpy(editor_status, "Save failed");
}

static void editor_handle_click(winrec_t *w, int mx, int my) {
    if (point_in(mx, my, w->x+4, w->y+w->h-16, 40, 12)) {
        editor_save();
    } else if (point_in(mx, my, w->x+48, w->y+w->h-16, 40, 12)) {
        if (!fat12_mounted()) { kstrcpy(editor_status, "No disk"); return; }
        uint8_t fbuf[EDIT_ROWS*(EDIT_COLS+1)];
        int n = fat12_read("NOTES.TXT", fbuf, sizeof(fbuf)-1);
        if (n < 0) { kstrcpy(editor_status, "Not found"); return; }
        fbuf[n] = 0;
        editor_init_buf();
        char *p = (char*)fbuf;
        while (*p && editor_row < EDIT_ROWS) {
            if (*p == '\n') { editor_row++; editor_col = 0; p++; continue; }
            if (editor_col < EDIT_COLS-1) {
                editor_lines[editor_row][editor_col++] = *p;
                editor_lines[editor_row][editor_col] = 0;
            }
            p++;
        }
        kstrcpy(editor_status, "Loaded!");
    }
}

#define WRITE_ROWS 40
#define WRITE_COLS 74
#define WRITE_NAME_MAX 32

static char write_lines[WRITE_ROWS][WRITE_COLS+1];
static int  write_row = 0, write_col = 0;
static char write_filename[WRITE_NAME_MAX] = "DOC.TXT";
static int  write_editing_name = 0;
static char write_status[24] = "";

static void write_init_buf(void) {
    for (int i=0;i<WRITE_ROWS;i++) write_lines[i][0] = 0;
    write_row = 0; write_col = 0;
}

static void write_counts(int *words, int *chars) {
    int w = 0, c = 0, in_word = 0;
    for (int i = 0; i <= write_row; i++) {
        int len = (i < write_row) ? (int)kstrlen(write_lines[i]) : write_col;
        for (int j = 0; j < len; j++) {
            char ch = write_lines[i][j];
            c++;
            if (ch == ' ' || ch == '\t') in_word = 0;
            else if (!in_word) { in_word = 1; w++; }
        }
        if (i < write_row) c++;
    }
    *words = w; *chars = c;
}

static void render_write(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "KumWrite");

    int cy = w->y + 12;
    gui_puts(w->x+3, cy+2, "File:", C_DARK_GREY, C_WIN_BG);
    gui_textfield_draw(w->x+34, cy, 120, 12, write_filename, write_editing_name);

    gui_button(w->x+158, cy, 36, 12, "New",  gui_pressed(w->x+158, cy, 36, 12));
    gui_button(w->x+196, cy, 40, 12, "Open", gui_pressed(w->x+196, cy, 40, 12));
    gui_button(w->x+238, cy, 36, 12, "Save", gui_pressed(w->x+238, cy, 36, 12));
    cy += 15;

    int content_y0 = cy;
    int content_h  = w->h - (content_y0 - w->y) - 12;
    if (content_h < 0) content_h = 0;
    gui_rect_fill(w->x+1, content_y0, w->w-2, content_h, C_WHITE);

    int rows = content_h / 9;
    for (int i = 0; i < WRITE_ROWS && i < rows; i++)
        gui_puts(w->x+3, content_y0+2+i*9, write_lines[i], C_BLACK, C_WHITE);
    if (!write_editing_name && (timer_ticks()/50) & 1)
        gui_rect_fill(w->x+3+write_col*8, content_y0+2+write_row*9, 7, 8, C_LIGHT_GREY);

    int words, chars; write_counts(&words, &chars);
    char statline[64];
    kstrcpy(statline, write_status);
    if (!statline[0]) {
        char nb[12]; kitoa((uint32_t)words, nb, 10);
        kstrcpy(statline, nb); kstrcat(statline, " words, ");
        kitoa((uint32_t)chars, nb, 10); kstrcat(statline, nb); kstrcat(statline, " chars");
    }
    gui_puts(w->x+3, w->y+w->h-10, statline, C_DARK_GREY, C_WIN_BG);
}

static void write_save(void) {
    if (!write_filename[0]) { kstrcpy(write_status, "Need a filename"); return; }
    char buf[WRITE_ROWS*(WRITE_COLS+1)]; int n=0;
    for (int i=0;i<=write_row;i++) {
        const char *p = write_lines[i];
        while (*p) buf[n++] = *p++;
        if (i < write_row) buf[n++] = '\n';
    }
    if (!fat12_mounted()) kstrcpy(write_status, "No disk");
    else if (fat12_write(write_filename, buf, (uint32_t)n) == 0) kstrcpy(write_status, "Saved!");
    else kstrcpy(write_status, "Save failed");
}

static void write_new(void) {
    write_init_buf();
    kstrcpy(write_status, "New doc");
}

static void write_open(void) {
    if (!fat12_mounted()) { kstrcpy(write_status, "No disk"); return; }
    uint8_t fbuf[WRITE_ROWS*(WRITE_COLS+1)];
    int n = fat12_read(write_filename, fbuf, sizeof(fbuf)-1);
    if (n < 0) { kstrcpy(write_status, "Not found"); return; }
    fbuf[n] = 0;
    write_init_buf();
    char *p = (char*)fbuf;
    while (*p && write_row < WRITE_ROWS) {
        if (*p == '\n') { write_row++; write_col = 0; p++; continue; }
        if (write_col < WRITE_COLS-1) {
            write_lines[write_row][write_col++] = *p;
            write_lines[write_row][write_col] = 0;
        }
        p++;
    }
    kstrcpy(write_status, "Loaded!");
}

static void write_handle_click(winrec_t *w, int mx, int my) {
    int cy = w->y + 12;
    if (point_in(mx, my, w->x+34, cy, 120, 12)) { write_editing_name = 1; return; }
    write_editing_name = 0;
    write_status[0] = 0;

    if (point_in(mx, my, w->x+158, cy, 36, 12)) {
        write_new();
    } else if (point_in(mx, my, w->x+196, cy, 40, 12)) {
        write_open();
    } else if (point_in(mx, my, w->x+238, cy, 36, 12)) {
        write_save();
    }
}

#define PAINT_W 300
#define PAINT_H 190

static uint8_t paint_canvas[PAINT_H][PAINT_W];
static int     paint_inited = 0;
static uint8_t paint_color = C_BLACK;
static int     g_painting = 0;

static const uint8_t paint_palette[8] = {
    C_BLACK, C_RED, C_LIGHT_GREEN, C_LIGHT_BLUE, C_YELLOW, C_WHITE, C_BROWN, C_PURPLE
};

static void paint_clear(void) {
    for (int y=0;y<PAINT_H;y++)
        for (int x=0;x<PAINT_W;x++)
            paint_canvas[y][x] = C_WHITE;
}

static void paint_save(void) {
    fat12_write("CANVAS.PNT", &paint_canvas[0][0], PAINT_H*PAINT_W);
}

static void paint_load(void) {
    fat12_read("CANVAS.PNT", &paint_canvas[0][0], PAINT_H*PAINT_W);
}

static void paint_dot(int lx, int ly) {
    for (int dy=-1; dy<=1; dy++)
        for (int dx=-1; dx<=1; dx++) {
            int x = lx+dx, y = ly+dy;
            if (x>=0 && x<PAINT_W && y>=0 && y<PAINT_H) paint_canvas[y][x] = paint_color;
        }
}

static void render_paint(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Paint");
    if (!paint_inited) { paint_clear(); paint_inited = 1; }

    int cx0 = w->x+4, cy0 = w->y+14;
    gui_rect(cx0-1, cy0-1, PAINT_W+2, PAINT_H+2, C_WIN_BORDER);
    for (int y=0;y<PAINT_H;y++) {
        int sy = cy0+y;
        if ((unsigned)sy >= GUI_HEIGHT) break;
        kmemcpy(backbuf + sy*GUI_WIDTH + cx0, paint_canvas[y], PAINT_W);
    }

    int py = cy0 + PAINT_H + 6;
    for (int i=0;i<8;i++) {
        int px = cx0 + i*20;
        gui_rect_fill(px, py, 16, 16, paint_palette[i]);
        gui_rect(px, py, 16, 16, paint_color==paint_palette[i] ? C_WHITE : C_WIN_BORDER);
    }
    gui_button(cx0+8*20+8, py, 40, 16, "Clear", gui_pressed(cx0+8*20+8, py, 40, 16));
    gui_button(cx0+8*20+52, py, 40, 16, "Save", gui_pressed(cx0+8*20+52, py, 40, 16));
    gui_button(cx0+8*20+96, py, 40, 16, "Load", gui_pressed(cx0+8*20+96, py, 40, 16));
}

static void paint_handle_click(winrec_t *w, int mx, int my) {
    int cx0 = w->x+4, cy0 = w->y+14;
    int py = cy0 + PAINT_H + 6;

    for (int i=0;i<8;i++) {
        int px = cx0 + i*20;
        if (point_in(mx, my, px, py, 16, 16)) { paint_color = paint_palette[i]; return; }
    }
    if (point_in(mx, my, cx0+8*20+8, py, 40, 16)) { paint_clear(); return; }
    if (point_in(mx, my, cx0+8*20+52, py, 40, 16)) { paint_save(); return; }
    if (point_in(mx, my, cx0+8*20+96, py, 40, 16)) { paint_load(); return; }
    if (point_in(mx, my, cx0, cy0, PAINT_W, PAINT_H)) {
        paint_dot(mx-cx0, my-cy0);
        g_painting = 1;
    }
}

#define SNAKE_COLS 20
#define SNAKE_ROWS 15
#define SNAKE_CELL 10
#define SNAKE_MAX_LEN (SNAKE_COLS*SNAKE_ROWS)

typedef struct { int8_t x, y; } snake_cell_t;
static snake_cell_t snake_body[SNAKE_MAX_LEN];
static int      snake_len = 0;
static int      snake_dx = 1, snake_dy = 0;
static int      snake_food_x = 0, snake_food_y = 0;
static int      snake_score = 0;
static int      snake_over = 0;
static int      snake_started = 0;
static uint32_t snake_last_tick = 0;
static int      snake_hi = 0;
static int      snake_hi_loaded = 0;

static void snake_load_hi(void) {
    if (snake_hi_loaded) return;
    snake_hi_loaded = 1;
    uint8_t buf[4];
    if (fat12_read("SNAKE.HI", buf, 4) == 4)
        snake_hi = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
}

static void snake_save_hi(void) {
    uint8_t buf[4] = { (uint8_t)snake_hi, (uint8_t)(snake_hi>>8),
                        (uint8_t)(snake_hi>>16), (uint8_t)(snake_hi>>24) };
    fat12_write("SNAKE.HI", buf, 4);
}

static void snake_place_food(void) {
    for (;;) {
        int fx = (int)(gui_rand() % SNAKE_COLS);
        int fy = (int)(gui_rand() % SNAKE_ROWS);
        int hit = 0;
        for (int i=0;i<snake_len;i++)
            if (snake_body[i].x==fx && snake_body[i].y==fy) { hit=1; break; }
        if (!hit) { snake_food_x = fx; snake_food_y = fy; return; }
    }
}

static void snake_reset(void) {
    snake_len = 3;
    snake_body[0] = (snake_cell_t){10,7};
    snake_body[1] = (snake_cell_t){9,7};
    snake_body[2] = (snake_cell_t){8,7};
    snake_dx = 1; snake_dy = 0;
    snake_score = 0;
    snake_over = 0;
    snake_last_tick = timer_ticks();
    snake_place_food();
}

static void snake_die(void) {
    snake_over = 1;
    if (snake_score > snake_hi) { snake_hi = snake_score; snake_save_hi(); }
}

static void snake_step(void) {
    int nx = snake_body[0].x + snake_dx;
    int ny = snake_body[0].y + snake_dy;
    if (nx<0 || nx>=SNAKE_COLS || ny<0 || ny>=SNAKE_ROWS) { snake_die(); return; }
    for (int i=0;i<snake_len;i++)
        if (snake_body[i].x==nx && snake_body[i].y==ny) { snake_die(); return; }

    int grow = (nx==snake_food_x && ny==snake_food_y);
    int new_len = grow ? snake_len+1 : snake_len;
    if (new_len > SNAKE_MAX_LEN) new_len = SNAKE_MAX_LEN;

    for (int i = new_len-1; i > 0; i--) snake_body[i] = snake_body[i-1];
    snake_body[0].x = (int8_t)nx; snake_body[0].y = (int8_t)ny;
    snake_len = new_len;
    if (grow) { snake_score++; snake_place_food(); }
}

static void render_snake(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Snake");
    snake_load_hi();
    if (!snake_started) { snake_reset(); snake_started = 1; }

    if (!snake_over && timer_ticks() - snake_last_tick >= 15) {
        snake_last_tick = timer_ticks();
        snake_step();
    }

    int cx0 = w->x+4, cy0 = w->y+14;
    int cw = SNAKE_COLS*SNAKE_CELL, ch = SNAKE_ROWS*SNAKE_CELL;
    gui_rect_fill(cx0, cy0, cw, ch, C_BLACK);
    gui_rect(cx0-1, cy0-1, cw+2, ch+2, C_WIN_BORDER);

    for (int i=0;i<snake_len;i++) {
        uint8_t c = (i==0) ? C_LIGHT_GREEN : C_GREEN;
        gui_rect_fill(cx0+snake_body[i].x*SNAKE_CELL, cy0+snake_body[i].y*SNAKE_CELL,
                      SNAKE_CELL-1, SNAKE_CELL-1, c);
    }
    gui_rect_fill(cx0+snake_food_x*SNAKE_CELL, cy0+snake_food_y*SNAKE_CELL,
                  SNAKE_CELL-1, SNAKE_CELL-1, C_RED);

    char scorebuf[24] = "Score: ";
    char nb[12]; kitoa((uint32_t)snake_score, nb, 10);
    kstrcat(scorebuf, nb);
    kstrcat(scorebuf, "  Best: ");
    kitoa((uint32_t)snake_hi, nb, 10);
    kstrcat(scorebuf, nb);
    gui_puts(cx0, cy0+ch+4, scorebuf, C_WHITE, C_WIN_BG);
    if (snake_over)
        gui_puts(cx0, cy0+ch+14, "Game over - press R", C_LIGHT_RED, C_WIN_BG);
    else
        gui_puts(cx0, cy0+ch+14, "Arrow keys to steer", C_DARK_GREY, C_WIN_BG);
}

#define PIANO_KEYS 8
static const uint32_t piano_freqs[PIANO_KEYS] = {262,294,330,349,392,440,494,523};
static const char     piano_names[PIANO_KEYS] = {'C','D','E','F','G','A','B','C'};
static int piano_active = -1;

#define PIANO_KW 26
#define PIANO_KH 100

static void render_piano(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Piano");
    int cx0 = w->x+6, cy0 = w->y+16;
    for (int i=0;i<PIANO_KEYS;i++) {
        int kx = cx0 + i*(PIANO_KW+2);
        uint8_t c = (piano_active==i) ? C_LIGHT_BLUE : C_WHITE;
        gui_rect_fill(kx, cy0, PIANO_KW, PIANO_KH, c);
        gui_rect(kx, cy0, PIANO_KW, PIANO_KH, C_DARK_GREY);
        char lbl[2] = { piano_names[i], 0 };
        gui_puts(kx+9, cy0+PIANO_KH-14, lbl, C_BLACK, c);
    }
    gui_puts(cx0, cy0+PIANO_KH+8, "Click and hold a key", C_DARK_GREY, C_WIN_BG);
}

static void piano_handle_click(winrec_t *w, int mx, int my) {
    int cx0 = w->x+6, cy0 = w->y+16;
    for (int i=0;i<PIANO_KEYS;i++) {
        int kx = cx0 + i*(PIANO_KW+2);
        if (point_in(mx, my, kx, cy0, PIANO_KW, PIANO_KH)) {
            piano_active = i;
            speaker_on(piano_freqs[i]);
            return;
        }
    }
}

static int g2048_board[4][4];
static int g2048_score = 0;
static int g2048_over  = 0;
static int g2048_started = 0;
static int g2048_hi = 0;
static int g2048_hi_loaded = 0;

static void g2048_load_hi(void) {
    if (g2048_hi_loaded) return;
    g2048_hi_loaded = 1;
    uint8_t buf[4];
    if (fat12_read("2048.HI", buf, 4) == 4)
        g2048_hi = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
}

static void g2048_save_hi(void) {
    uint8_t buf[4] = { (uint8_t)g2048_hi, (uint8_t)(g2048_hi>>8),
                        (uint8_t)(g2048_hi>>16), (uint8_t)(g2048_hi>>24) };
    fat12_write("2048.HI", buf, 4);
}

static void g2048_spawn(void) {
    int er[16], ec[16], n=0;
    for (int r=0;r<4;r++) for (int c=0;c<4;c++)
        if (!g2048_board[r][c]) { er[n]=r; ec[n]=c; n++; }
    if (!n) return;
    int pick = (int)(gui_rand() % (uint32_t)n);
    g2048_board[er[pick]][ec[pick]] = (gui_rand()%10==0) ? 4 : 2;
}

static void g2048_reset(void) {
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) g2048_board[r][c] = 0;
    g2048_score = 0; g2048_over = 0;
    g2048_spawn(); g2048_spawn();
}

static int g2048_line_merge(int line[4]) {
    int tmp[4], n=0;
    for (int i=0;i<4;i++) if (line[i]) tmp[n++] = line[i];
    int out[4] = {0,0,0,0}, oi=0;
    for (int i=0;i<n;i++) {
        if (i+1<n && tmp[i]==tmp[i+1]) {
            out[oi] = tmp[i]*2;
            g2048_score += out[oi];
            oi++; i++;
        } else {
            out[oi++] = tmp[i];
        }
    }
    int changed = 0;
    for (int i=0;i<4;i++) { if (out[i]!=line[i]) changed=1; line[i]=out[i]; }
    return changed;
}

static int g2048_no_moves_left(void) {
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) {
        if (!g2048_board[r][c]) return 0;
        if (c<3 && g2048_board[r][c]==g2048_board[r][c+1]) return 0;
        if (r<3 && g2048_board[r][c]==g2048_board[r+1][c]) return 0;
    }
    return 1;
}

static void g2048_move(int dir) {
    int changed_any = 0;
    if (dir==0 || dir==1) {
        for (int r=0;r<4;r++) {
            int line[4];
            for (int c=0;c<4;c++) line[c] = g2048_board[r][dir==0 ? c : 3-c];
            if (g2048_line_merge(line)) changed_any = 1;
            for (int c=0;c<4;c++) g2048_board[r][dir==0 ? c : 3-c] = line[c];
        }
    } else {
        for (int c=0;c<4;c++) {
            int line[4];
            for (int r=0;r<4;r++) line[r] = g2048_board[dir==2 ? r : 3-r][c];
            if (g2048_line_merge(line)) changed_any = 1;
            for (int r=0;r<4;r++) g2048_board[dir==2 ? r : 3-r][c] = line[r];
        }
    }
    if (changed_any) {
        g2048_spawn();
        if (g2048_no_moves_left()) {
            g2048_over = 1;
            if (g2048_score > g2048_hi) { g2048_hi = g2048_score; g2048_save_hi(); }
        }
    }
}

static uint8_t g2048_tile_color(int v) {
    switch (v) {
        case 0:    return C_LIGHT_GREY;
        case 2:    return C_WHITE;
        case 4:    return C_YELLOW;
        case 8:    return C_ORANGE;
        case 16:   return C_LIGHT_RED;
        case 32:   return C_RED;
        case 64:   return C_PURPLE;
        case 128:  return C_LIGHT_BLUE;
        case 256:  return C_LIGHT_GREEN;
        case 512:  return C_GREEN;
        case 1024: return C_TEAL;
        default:   return C_NAVY;
    }
}

#define G2048_CELL 44
#define G2048_GAP   4

static void render_2048(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "2048");
    g2048_load_hi();
    if (!g2048_started) { g2048_reset(); g2048_started = 1; }

    int cx0 = w->x+6, cy0 = w->y+16;
    for (int r=0;r<4;r++) {
        for (int c=0;c<4;c++) {
            int v = g2048_board[r][c];
            int tx = cx0 + c*(G2048_CELL+G2048_GAP);
            int ty = cy0 + r*(G2048_CELL+G2048_GAP);
            gui_rect_fill(tx, ty, G2048_CELL, G2048_CELL, g2048_tile_color(v));
            gui_rect(tx, ty, G2048_CELL, G2048_CELL, C_WIN_BORDER);
            if (v) {
                char nb[8]; kitoa((uint32_t)v, nb, 10);
                int tw = (int)kstrlen(nb)*8;
                gui_puts(tx+(G2048_CELL-tw)/2, ty+(G2048_CELL-8)/2, nb,
                         v<=4 ? C_BLACK : C_WHITE, g2048_tile_color(v));
            }
        }
    }

    int by = cy0 + 4*(G2048_CELL+G2048_GAP) + 4;
    char scorebuf[24] = "Score: ";
    char nb[12]; kitoa((uint32_t)g2048_score, nb, 10);
    kstrcat(scorebuf, nb);
    kstrcat(scorebuf, "  Best: ");
    kitoa((uint32_t)g2048_hi, nb, 10);
    kstrcat(scorebuf, nb);
    gui_puts(cx0, by, scorebuf, C_WHITE, C_WIN_BG);
    if (g2048_over)
        gui_puts(cx0, by+10, "Game over - press R", C_LIGHT_RED, C_WIN_BG);
    else
        gui_puts(cx0, by+10, "Arrow keys to slide", C_DARK_GREY, C_WIN_BG);
}

#define MINE_COLS  9
#define MINE_ROWS  9
#define MINE_MINES 10
#define MINE_CELL  16

static int8_t mine_board[MINE_ROWS][MINE_COLS];
static uint8_t mine_revealed[MINE_ROWS][MINE_COLS];
static uint8_t mine_flagged[MINE_ROWS][MINE_COLS];
static int mine_started = 0;
static int mine_over = 0;
static int mine_wins = 0;
static int mine_wins_loaded = 0;

static void mine_load_wins(void) {
    if (mine_wins_loaded) return;
    mine_wins_loaded = 1;
    uint8_t buf[4];
    if (fat12_read("MINES.WIN", buf, 4) == 4)
        mine_wins = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
}

static void mine_save_wins(void) {
    uint8_t buf[4] = { (uint8_t)mine_wins, (uint8_t)(mine_wins>>8),
                        (uint8_t)(mine_wins>>16), (uint8_t)(mine_wins>>24) };
    fat12_write("MINES.WIN", buf, 4);
}

static void mine_reset(void) {
    for (int r=0;r<MINE_ROWS;r++)
        for (int c=0;c<MINE_COLS;c++) {
            mine_board[r][c] = 0;
            mine_revealed[r][c] = 0;
            mine_flagged[r][c] = 0;
        }
    mine_over = 0;

    int placed = 0;
    while (placed < MINE_MINES) {
        int r = (int)(gui_rand() % MINE_ROWS);
        int c = (int)(gui_rand() % MINE_COLS);
        if (mine_board[r][c] == -1) continue;
        mine_board[r][c] = -1;
        placed++;
    }

    for (int r=0;r<MINE_ROWS;r++)
        for (int c=0;c<MINE_COLS;c++) {
            if (mine_board[r][c] == -1) continue;
            int n = 0;
            for (int dr=-1;dr<=1;dr++)
                for (int dc=-1;dc<=1;dc++) {
                    if (!dr && !dc) continue;
                    int rr=r+dr, cc=c+dc;
                    if (rr<0||rr>=MINE_ROWS||cc<0||cc>=MINE_COLS) continue;
                    if (mine_board[rr][cc] == -1) n++;
                }
            mine_board[r][c] = (int8_t)n;
        }
}

static void mine_flood(int r0, int c0) {
    int sr[MINE_ROWS*MINE_COLS*9], sc[MINE_ROWS*MINE_COLS*9], sp = 0;
    sr[sp]=r0; sc[sp]=c0; sp++;
    while (sp > 0) {
        sp--;
        int r = sr[sp], c = sc[sp];
        if (r<0||r>=MINE_ROWS||c<0||c>=MINE_COLS) continue;
        if (mine_revealed[r][c] || mine_flagged[r][c]) continue;
        mine_revealed[r][c] = 1;
        if (mine_board[r][c] != 0) continue;
        for (int dr=-1;dr<=1;dr++)
            for (int dc=-1;dc<=1;dc++) {
                if (!dr && !dc) continue;
                sr[sp]=r+dr; sc[sp]=c+dc; sp++;
            }
    }
}

static int mine_check_win(void) {
    for (int r=0;r<MINE_ROWS;r++)
        for (int c=0;c<MINE_COLS;c++)
            if (mine_board[r][c] != -1 && !mine_revealed[r][c]) return 0;
    return 1;
}

static void mine_reveal(int r, int c) {
    if (mine_over || mine_flagged[r][c] || mine_revealed[r][c]) return;
    if (mine_board[r][c] == -1) {
        mine_revealed[r][c] = 1;
        mine_over = 1;
        gui_beep(110, 40);
        return;
    }
    mine_flood(r, c);
    if (mine_check_win()) {
        mine_over = 2;
        gui_beep(880, 30);
        mine_wins++;
        mine_save_wins();
    }
}

static const uint8_t mine_num_color[9] = {
    C_BLACK, C_BLUE, C_LIGHT_GREEN, C_RED, C_NAVY, C_BROWN, C_TEAL, C_BLACK, C_DARK_GREY
};

static void render_mine(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Minesweeper");
    mine_load_wins();
    if (!mine_started) { mine_reset(); mine_started = 1; }

    int cx0 = w->x+4, cy0 = w->y+14;
    int flagged = 0;
    for (int r=0;r<MINE_ROWS;r++) {
        for (int c=0;c<MINE_COLS;c++) {
            int tx = cx0 + c*MINE_CELL, ty = cy0 + r*MINE_CELL;
            if (mine_flagged[r][c]) flagged++;

            if (!mine_revealed[r][c]) {
                gui_rect_fill(tx, ty, MINE_CELL-1, MINE_CELL-1, C_LIGHT_GREY);
                gui_rect(tx, ty, MINE_CELL-1, MINE_CELL-1, C_DARK_GREY);
                if (mine_flagged[r][c]) {
                    gui_vline(tx+7, ty+3, 9, C_BLACK);
                    gui_rect_fill(tx+4, ty+3, 5, 4, C_RED);
                }
            } else if (mine_board[r][c] == -1) {
                gui_rect_fill(tx, ty, MINE_CELL-1, MINE_CELL-1, C_RED);
                gui_rect_fill(tx+5, ty+5, 5, 5, C_BLACK);
            } else {
                gui_rect_fill(tx, ty, MINE_CELL-1, MINE_CELL-1, C_WIN_BG);
                gui_rect(tx, ty, MINE_CELL-1, MINE_CELL-1, C_DARK_GREY);
                int v = mine_board[r][c];
                if (v > 0) {
                    char nb[2] = { (char)('0'+v), 0 };
                    gui_puts(tx+5, ty+4, nb, mine_num_color[v], C_WIN_BG);
                }
            }
        }
    }

    int by = cy0 + MINE_ROWS*MINE_CELL + 4;
    char statusbuf[24] = "Mines left: ";
    char nb[12]; kitoa((uint32_t)(MINE_MINES-flagged), nb, 10);
    kstrcat(statusbuf, nb);
    gui_puts(cx0, by, statusbuf, C_WHITE, C_WIN_BG);
    if (mine_over == 1)
        gui_puts(cx0, by+10, "Boom - press R to retry", C_LIGHT_RED, C_WIN_BG);
    else if (mine_over == 2)
        gui_puts(cx0, by+10, "Cleared! Press R to play again", C_LIGHT_GREEN, C_WIN_BG);
    else
        gui_puts(cx0, by+10, "Left: dig  Right: flag", C_DARK_GREY, C_WIN_BG);

    char winsbuf[16] = "Wins: ";
    kitoa((uint32_t)mine_wins, nb, 10);
    kstrcat(winsbuf, nb);
    gui_puts(cx0, by+20, winsbuf, C_DARK_GREY, C_WIN_BG);
}

static void mine_handle_click(winrec_t *w, int mx, int my) {
    int cx0 = w->x+4, cy0 = w->y+14;
    if (mx < cx0 || my < cy0) return;
    int c = (mx-cx0)/MINE_CELL, r = (my-cy0)/MINE_CELL;
    if (r>=MINE_ROWS||c>=MINE_COLS) return;
    mine_reveal(r, c);
}

#define SETTINGS_SWATCH_W 70
#define SETTINGS_SWATCH_H 40
#define SETTINGS_SWATCH_GAP 12

static void settings_swatch_rect(winrec_t *w, int i, int *sx, int *sy) {
    *sx = w->x + 14 + i*(SETTINGS_SWATCH_W + SETTINGS_SWATCH_GAP);
    *sy = w->y + 40;
}

static void render_settings(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Settings");
    gui_puts(w->x+10, w->y+16, "Desktop theme:", C_WHITE, C_WIN_BG);

    for (int i = 0; i < GUI_THEME_COUNT; i++) {
        int sx, sy; settings_swatch_rect(w, i, &sx, &sy);

        uint8_t approx = (i==0) ? C_LIGHT_BLUE : (i==1) ? C_ORANGE : C_LIGHT_GREEN;
        gui_rect_fill(sx, sy, SETTINGS_SWATCH_W, SETTINGS_SWATCH_H, approx);
        gui_rect(sx, sy, SETTINGS_SWATCH_W, SETTINGS_SWATCH_H,
                 (i == gui_theme) ? C_WHITE : C_DARK_GREY);
        gui_puts(sx+4, sy+SETTINGS_SWATCH_H+2, gui_theme_names[i],
                 (i == gui_theme) ? C_YELLOW : C_LIGHT_GREY, C_WIN_BG);
    }

    gui_puts(w->x+10, w->y+w->h-24, "Click a swatch to apply it.",
             C_DARK_GREY, C_WIN_BG);
}

static void settings_handle_click(winrec_t *w, int mx, int my) {
    for (int i = 0; i < GUI_THEME_COUNT; i++) {
        int sx, sy; settings_swatch_rect(w, i, &sx, &sy);
        if (mx >= sx && mx < sx+SETTINGS_SWATCH_W && my >= sy && my < sy+SETTINGS_SWATCH_H) {
            gui_theme = i;
            gui_apply_theme();
            gui_save_settings();
            return;
        }
    }
}

static void mine_handle_rightclick(winrec_t *w, int mx, int my) {
    int cx0 = w->x+4, cy0 = w->y+14;
    if (mx < cx0 || my < cy0) return;
    int c = (mx-cx0)/MINE_CELL, r = (my-cy0)/MINE_CELL;
    if (r>=MINE_ROWS||c>=MINE_COLS) return;
    if (mine_over || mine_revealed[r][c]) return;
    mine_flagged[r][c] = !mine_flagged[r][c];
}

static int  pkg_selected = -1;
static char pkg_status[24] = "";

static void pkg_refresh(void) { pkg_selected = -1; pkg_status[0] = 0; }

static int pkgwin_total(void) { return pkg_count() + pkgnet_count(); }

static const char *pkgwin_name(int i) {
    int n = pkg_count();
    return (i < n) ? pkg_name_at(i) : pkgnet_name_at(i-n);
}

static int pkgwin_installed(int i) {
    int n = pkg_count();
    if (i < n) return pkg_is_installed(i);
    if (!fat12_mounted()) return 0;
    fat12_entry_t ents[64];
    int cnt = fat12_list(ents, 64);
    const char *fname = pkgnet_fname_at(i-n);
    for (int j=0;j<cnt;j++) if (kstrcmp(ents[j].name, fname)==0) return 1;
    return 0;
}

#define PKGWIN_ROW_H 9

static void render_pkg(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Packages");
    gui_rect_fill(w->x+1, w->y+12, w->w-2, w->h-13, C_BLACK);

    int cx = w->x+3, cy = w->y+13;
    int total = pkgwin_total();
    int by = w->y+w->h-20;
    int maxrows = (by - cy) / PKGWIN_ROW_H;

    for (int i=0;i<total && i<maxrows;i++) {
        int ry = cy + i*PKGWIN_ROW_H - 1;
        uint8_t fg = (i==pkg_selected) ? C_BLACK : (pkgwin_installed(i) ? C_LIGHT_GREEN : C_LIGHT_GREY);
        uint8_t bg = (i==pkg_selected) ? (uint8_t)(C_GRAD_ACCENT+3) : C_BLACK;
        if (i==pkg_selected) gui_rect_fill(w->x+1, ry, w->w-2, PKGWIN_ROW_H, bg);
        gui_puts(cx, cy+i*PKGWIN_ROW_H, pkgwin_name(i), fg, bg);
    }

    gui_button(w->x+3,   by, 44, 11, "Update", gui_pressed(w->x+3,   by, 44, 11));
    gui_button(w->x+49,  by, 36, 11, "Instl",  gui_pressed(w->x+49,  by, 36, 11));
    gui_button(w->x+87,  by, 40, 11, "Rmove",  gui_pressed(w->x+87,  by, 40, 11));
    gui_button(w->x+129, by, 36, 11, "Run",    gui_pressed(w->x+129, by, 36, 11));
    gui_puts(w->x+3, by+12, pkg_status, C_YELLOW, C_WIN_BG);
}

static void pkg_handle_click(winrec_t *w, int mx, int my) {
    int cy = w->y+13;
    int total = pkgwin_total();
    int by = w->y+w->h-20;
    int maxrows = (by - cy) / PKGWIN_ROW_H;

    if (point_in(mx, my, w->x+3, by, 44, 11)) {
        int n = pkgnet_update();
        kstrcpy(pkg_status, n < 0 ? "update failed" : "index updated");
        return;
    }
    if (point_in(mx, my, w->x+49, by, 40, 11)) {
        if (pkg_selected >= 0) {
            const char *nm = pkgwin_name(pkg_selected);
            int r = pkg_install(nm);
            if (r == -1) r = pkgnet_install(nm);
            kstrcpy(pkg_status, r == 0 ? "installed" : "install failed");
        }
        return;
    }
    if (point_in(mx, my, w->x+87, by, 40, 11)) {
        if (pkg_selected >= 0) {
            int r = pkg_remove(pkgwin_name(pkg_selected));
            kstrcpy(pkg_status, r == 0 ? "removed" : "remove failed");
        }
        return;
    }
    if (point_in(mx, my, w->x+129, by, 36, 11)) {
        if (pkg_selected < 0) { kstrcpy(pkg_status, "select one first"); return; }
        if (!pkgwin_installed(pkg_selected)) { kstrcpy(pkg_status, "install it first"); return; }

        term_run_command(pkgwin_name(pkg_selected));
        kstrcpy(pkg_status, "started in a terminal");
        return;
    }
    for (int i=0;i<total && i<maxrows;i++) {
        int ry = cy + i*PKGWIN_ROW_H - 1;
        if (point_in(mx, my, w->x+1, ry, w->w-2, PKGWIN_ROW_H)) { pkg_selected = i; return; }
    }
}

#define BROWSER_URL_MAX 128
static char browser_urlbar[BROWSER_URL_MAX] = "https://www.ecosia.org/";
static int  browser_editing = 1;
static int  browser_scroll  = 0;

static void render_browser(winrec_t *w) {
    gui_window(w->x, w->y, w->w, w->h, "Browser");

    int cy = w->y + 12;
    gui_textfield_draw(w->x+3, cy, w->w-46, 12, browser_urlbar, browser_editing);
    gui_button(w->x+w->w-40, cy, 38, 12, "Go", gui_pressed(w->x+w->w-40, cy, 38, 12));
    cy += 14;

    gui_puts(w->x+3, cy, browser_status(), C_DARK_GREY, C_WIN_BG);
    cy += 10;

    int content_y0 = cy;
    int content_h  = w->h - (content_y0 - w->y) - 4;
    if (content_h < 0) content_h = 0;
    gui_rect_fill(w->x+1, content_y0, w->w-2, content_h, C_WHITE);

    int rows  = content_h / 9;
    int total = browser_line_count();
    int maxscroll = total - rows; if (maxscroll < 0) maxscroll = 0;
    if (browser_scroll > maxscroll) browser_scroll = maxscroll;
    if (browser_scroll < 0) browser_scroll = 0;

    if (total == 0) {
        gui_puts(w->x+3, content_y0+2, "Type a URL above and press Enter.", C_DARK_GREY, C_WHITE);
    } else {
        for (int i = 0; i < rows; i++) {
            int li = browser_scroll + i;
            if (li >= total) break;
            gui_puts(w->x+3, content_y0+2+i*9, browser_line_at(li), C_BLACK, C_WHITE);
        }
    }
}

static void browser_go(void) {
    browser_editing = 0;
    browser_scroll = 0;
    browser_navigate(browser_urlbar);
    kstrncpy(browser_urlbar, browser_url(), BROWSER_URL_MAX-1);
    browser_urlbar[BROWSER_URL_MAX-1] = 0;
}

static void browser_handle_click(winrec_t *w, int mx, int my) {
    int cy = w->y + 12;
    if (point_in(mx, my, w->x+3, cy, w->w-46, 12)) { browser_editing = 1; return; }
    if (point_in(mx, my, w->x+w->w-40, cy, 38, 12)) { browser_go(); return; }
    browser_editing = 0;
}

#define LAUNCH_ROW_H 16
#define LAUNCH_W     168

#define LAUNCH_MAX   48

typedef struct { const char *label; icon_kind_t icon; int wintype; int is_pkg; int pkg_idx; } launch_item_t;

static int launcher_open = 0;

static int launcher_build(launch_item_t *items, int max) {
    int n = 0;
    if (n<max) items[n++] = (launch_item_t){"Terminal",       ICON_TERM,    WIN_TERMINAL, 0, 0};
    if (n<max) items[n++] = (launch_item_t){"Files",          ICON_FOLDER,  WIN_FILES,    0, 0};
    if (n<max) items[n++] = (launch_item_t){"System Monitor", ICON_MONITOR, WIN_SYSMON,   0, 0};
    if (n<max) items[n++] = (launch_item_t){"Calculator",     ICON_CALC,    WIN_CALC,     0, 0};
    if (n<max) items[n++] = (launch_item_t){"Notepad",        ICON_NOTE,    WIN_EDITOR,   0, 0};
    if (n<max) items[n++] = (launch_item_t){"Packages",       ICON_PKG,     WIN_PKG,      0, 0};
    if (n<max) items[n++] = (launch_item_t){"Browser",        ICON_BROWSER, WIN_BROWSER,  0, 0};
    if (n<max) items[n++] = (launch_item_t){"KumWrite",       ICON_NOTE,    WIN_WRITE,    0, 0};
    if (n<max) items[n++] = (launch_item_t){"Paint",          ICON_PAINT,   WIN_PAINT,    0, 0};
    if (n<max) items[n++] = (launch_item_t){"Snake",          ICON_SNAKE,   WIN_SNAKE,    0, 0};
    if (n<max) items[n++] = (launch_item_t){"Piano",          ICON_PIANO,   WIN_PIANO,    0, 0};
    if (n<max) items[n++] = (launch_item_t){"2048",           ICON_2048,    WIN_2048,     0, 0};
    if (n<max) items[n++] = (launch_item_t){"Minesweeper",    ICON_MINE,    WIN_MINE,     0, 0};
    if (n<max) items[n++] = (launch_item_t){"Settings",       ICON_SETTINGS,WIN_SETTINGS, 0, 0};
    int total = pkgwin_total();
    for (int i=0;i<total && n<max;i++) {
        if (!pkgwin_installed(i)) continue;
        items[n].label = pkgwin_name(i);
        items[n].icon = ICON_PKG;
        items[n].wintype = -1;
        items[n].is_pkg = 1;
        items[n].pkg_idx = i;
        n++;
    }
    return n;
}

static void launcher_open_wintype(int t) {
    switch (t) {
        case WIN_TERMINAL: wm_open(WIN_TERMINAL, 40,  20, TERM_W, TERM_H); break;
        case WIN_FILES:    wm_open(WIN_FILES,   760,  20, 230, 290);       break;
        case WIN_SYSMON:   wm_open(WIN_SYSMON,  760, 330, 230, 290);       break;
        case WIN_CALC:     wm_open(WIN_CALC,    480, 500, 190, 230);       break;
        case WIN_EDITOR:   wm_open(WIN_EDITOR,   40, 500, 420, 230);       break;
        case WIN_PKG:      wm_open(WIN_PKG,     690, 500, 300, 230);       break;
        case WIN_BROWSER:  wm_open(WIN_BROWSER, 260,  60, 500, 420);       break;
        case WIN_WRITE:    wm_open(WIN_WRITE,   300, 100, 520, 400);       break;
        case WIN_PAINT:    wm_open(WIN_PAINT,   340,  80, 340, 260);       break;
        case WIN_SNAKE:    wm_open(WIN_SNAKE,   400, 200, 220, 216);       break;
        case WIN_PIANO:    wm_open(WIN_PIANO,   420, 300, 240, 150);       break;
        case WIN_2048:     wm_open(WIN_2048,    460, 150, 220, 260);       break;
        case WIN_MINE:     wm_open(WIN_MINE,    430, 140, 160, 190);       break;
        case WIN_SETTINGS: wm_open(WIN_SETTINGS,420, 220, 260, 190);       break;
    }
}

static void launcher_run_pkg(int pkg_idx) {
    term_run_command(pkgwin_name(pkg_idx));
}

static void draw_launcher(void) {
    launch_item_t items[LAUNCH_MAX];
    int n = launcher_build(items, LAUNCH_MAX);
    int h = n*LAUNCH_ROW_H + 6;
    int x = 1, y = 11;

    fill_rounded(x+2, y+2, LAUNCH_W, h, C_SHADOW);
    fill_rounded(x, y, LAUNCH_W, h, C_WIN_BG);
    outline_rounded(x, y, LAUNCH_W, h, C_WIN_BORDER);

    for (int i=0;i<n;i++) {
        int ry = y+3+i*LAUNCH_ROW_H;
        if (i == 6 && n > 6) gui_hline(x+4, ry-2, LAUNCH_W-8, C_WIN_BORDER);
        icon_glyph(x+6, ry+1, items[i].icon);
        gui_puts(x+30, ry+4, items[i].label, C_WHITE, C_WIN_BG);
    }
}

static int launcher_hit(int mx, int my, int n) {
    int h = n*LAUNCH_ROW_H + 6;
    if (!point_in(mx, my, 1, 11, LAUNCH_W, h)) return -2;
    int row = (my - 14) / LAUNCH_ROW_H;
    return (row >= 0 && row < n) ? row : -1;
}

#define CTXMENU_ROW_H 16
#define CTXMENU_W     160
#define CTXMENU_ITEMS 6

static int ctxmenu_open = 0;
static int ctxmenu_x = 0, ctxmenu_y = 0;
static const char *ctxmenu_labels[CTXMENU_ITEMS] = {"New Terminal","Cascade Windows","Tile Windows","Close All Windows","Restart","Shut Down"};

static void ctxmenu_rect(int *x, int *y, int *h) {
    *h = CTXMENU_ITEMS*CTXMENU_ROW_H + 6;
    *x = ctxmenu_x; *y = ctxmenu_y;
    if (*x + CTXMENU_W > GUI_WIDTH)  *x = GUI_WIDTH  - CTXMENU_W;
    if (*y + *h > GUI_HEIGHT)        *y = GUI_HEIGHT - *h;
}

static void draw_ctxmenu(void) {
    int x, y, h; ctxmenu_rect(&x, &y, &h);
    fill_rounded(x+2, y+2, CTXMENU_W, h, C_SHADOW);
    fill_rounded(x, y, CTXMENU_W, h, C_WIN_BG);
    outline_rounded(x, y, CTXMENU_W, h, C_WIN_BORDER);
    for (int i=0;i<CTXMENU_ITEMS;i++) {
        int ry = y+3+i*CTXMENU_ROW_H;
        gui_puts(x+8, ry+4, ctxmenu_labels[i], C_WHITE, C_WIN_BG);
    }
}

#define CONFIRM_W 190
#define CONFIRM_H 54

static void confirm_rect(int *x, int *y) {
    *x = (GUI_WIDTH  - CONFIRM_W) / 2;
    *y = (GUI_HEIGHT - CONFIRM_H) / 2;
}

static void draw_confirm(void) {
    if (confirm_pending == CONFIRM_NONE) return;
    int x, y; confirm_rect(&x, &y);
    fill_rounded(x+2, y+2, CONFIRM_W, CONFIRM_H, C_SHADOW);
    fill_rounded(x, y, CONFIRM_W, CONFIRM_H, C_WIN_BG);
    outline_rounded(x, y, CONFIRM_W, CONFIRM_H, C_WIN_BORDER);

    char msg[64];
    if (confirm_pending == CONFIRM_DELETE_FILE) {
        kstrcpy(msg, "Delete "); kstrcat(msg, confirm_target_name); kstrcat(msg, "?");
    } else if (confirm_pending == CONFIRM_KILL_TASK) {
        kstrcpy(msg, "Kill this task?");
    } else if (confirm_pending == CONFIRM_REBOOT) {
        kstrcpy(msg, "Restart KumOS?");
    } else {
        kstrcpy(msg, "Shut down KumOS?");
    }
    gui_puts(x+10, y+9, msg, C_WHITE, C_WIN_BG);

    int by = y+CONFIRM_H-20;
    gui_button(x+18, by, 60, 14, "Yes", gui_pressed(x+18, by, 60, 14));
    gui_button(x+CONFIRM_W-78, by, 60, 14, "No", gui_pressed(x+CONFIRM_W-78, by, 60, 14));
}

static int confirm_handle_click(int mx, int my) {
    if (confirm_pending == CONFIRM_NONE) return 0;
    int x, y; confirm_rect(&x, &y);
    int by = y+CONFIRM_H-20;
    if (point_in(mx, my, x+18, by, 60, 14)) {
        if (confirm_pending == CONFIRM_DELETE_FILE) {
            fat12_delete(confirm_target_name);
            files_refresh();
        } else if (confirm_pending == CONFIRM_KILL_TASK) {
            signal_send(confirm_target_pid, SIGTERM);
        } else if (confirm_pending == CONFIRM_REBOOT) {
            machine_reboot();
        } else if (confirm_pending == CONFIRM_POWEROFF) {
            for (int i = 0; i < VGA_TTYS; i++) term_close_session(i);
            machine_poweroff();
        }
        confirm_pending = CONFIRM_NONE;
    } else if (point_in(mx, my, x+CONFIRM_W-78, by, 60, 14)) {
        confirm_pending = CONFIRM_NONE;
    }
    return 1;
}

static int ctxmenu_hit(int mx, int my) {
    int x, y, h; ctxmenu_rect(&x, &y, &h);
    if (!point_in(mx, my, x, y, CTXMENU_W, h)) return -2;
    int row = (my - (y+3)) / CTXMENU_ROW_H;
    return (row >= 0 && row < CTXMENU_ITEMS) ? row : -1;
}

void gui_run(void) {
    gui_init();
    editor_init_buf();

    for (int i=0;i<WIN_COUNT;i++) wins[i].active = 0;
    zcount = 0;
    wm_open(WIN_TERMINAL, 40, 20, TERM_W, TERM_H);
    serial_printf("[boot] GUI started\r\n");

    int prev_left = 0, prev_right = 0;
    int dragging = -1, drag_ox = 0, drag_oy = 0;
    int last_tb_win = -1; uint32_t last_tb_tick = 0;
    int resizing = -1, resize_ow = 0, resize_oh = 0, resize_mx = 0, resize_my = 0;
    int running = 1;

    gui_draw_desktop();
    kmemcpy(desktop_cache, backbuf, GUI_WIDTH * GUI_HEIGHT);

    while (running) {
        kmemcpy(backbuf, desktop_cache, GUI_WIDTH * GUI_HEIGHT);

        for (int i=0;i<zcount;i++) {
            int t = zorder[i];
            winrec_t *w = &wins[t];
            if (!w->active || w->minimized) continue;
            if (t>=WIN_TERMINAL && t<=WIN_TERMINAL3) render_terminal(w, t - WIN_TERMINAL);
            else if (t==WIN_FILES) render_files(w);
            else if (t==WIN_SYSMON) render_sysmon(w);
            else if (t==WIN_CALC) render_calc(w);
            else if (t==WIN_EDITOR) render_editor(w);
            else if (t==WIN_PKG) render_pkg(w);
            else if (t==WIN_BROWSER) render_browser(w);
            else if (t==WIN_WRITE) render_write(w);
            else if (t==WIN_PAINT) render_paint(w);
            else if (t==WIN_SNAKE) render_snake(w);
            else if (t==WIN_PIANO) render_piano(w);
            else if (t==WIN_2048) render_2048(w);
            else if (t==WIN_MINE) render_mine(w);
            else if (t==WIN_SETTINGS) render_settings(w);
        }

        gui_draw_taskbar();
        if (launcher_open) draw_launcher();
        if (ctxmenu_open) draw_ctxmenu();
        draw_confirm();

        mouse_state_t *m = mouse_get();
        uint8_t latched = mouse_take_pressed();
        int mx = m->x, my = m->y;
        if (mx < 0) mx = 0; if (mx >= GUI_WIDTH)  mx = GUI_WIDTH-1;
        if (my < 0) my = 0; if (my >= GUI_HEIGHT) my = GUI_HEIGHT-1;

        cur_mx = mx; cur_my = my; cur_mdown = m->left;

        gui_draw_cursor(mx, my);
        gui_flip();

        int ftty = focused_tty();
        int host_focused = ftty >= 0 && term_alive(ftty);
        char key = 0;
        if (host_focused) {
            if (keyboard_check_alttab()) wm_cycle_focus();
        } else {
            key = keyboard_getchar();
        }
        if (key) {
            int top = wm_topmost();
            if (ftty >= 0 && !term_alive(ftty)) {
                term_open_session(ftty);
            } else if (key == 27) {
                if (ctxmenu_open) ctxmenu_open = 0;
                else if (launcher_open) launcher_open = 0;
                else if (top >= 0) wm_close(top);
                else running = 0;
            } else if (key == '\t' && keyboard_alt_held()) {
                wm_cycle_focus();
            } else if (key == 19 && top == WIN_EDITOR) {
                editor_save();
            } else if (key == 19 && top == WIN_WRITE) {
                write_save();
            } else if (key == 14 && top == WIN_WRITE) {
                write_new();
            } else if (key == 15 && top == WIN_WRITE) {
                write_open();
            } else if (top == WIN_EDITOR) {
                if (key == '\n') {
                    if (editor_row < EDIT_ROWS-1) {
                        editor_row++; editor_col = 0; editor_lines[editor_row][0] = 0;
                    }
                } else if (key == '\b') {
                    if (editor_col > 0) {
                        editor_col--; editor_lines[editor_row][editor_col] = 0;
                    } else if (editor_row > 0) {
                        editor_row--; editor_col = (int)kstrlen(editor_lines[editor_row]);
                    }
                } else if (editor_col < EDIT_COLS-1) {
                    editor_lines[editor_row][editor_col++] = key;
                    editor_lines[editor_row][editor_col]   = 0;
                }
            } else if (top == WIN_BROWSER) {
                if (browser_editing) {
                    if (key == '\n') browser_go();
                    else gui_textfield_key(browser_urlbar, BROWSER_URL_MAX, key);
                } else {
                    if (key == KEY_UP) { if (browser_scroll > 0) browser_scroll--; }
                    else if (key == KEY_DOWN) browser_scroll++;
                    else if (key == ' ') browser_scroll += 15;
                }
            } else if (top == WIN_FILES && files_renaming) {
                if (key == '\n') files_do_rename();
                else gui_textfield_key(files_renamebuf, sizeof(files_renamebuf), key);
            } else if (top == WIN_WRITE) {
                if (write_editing_name) {
                    if (key == '\n') write_editing_name = 0;
                    else gui_textfield_key(write_filename, WRITE_NAME_MAX, key);
                } else if (key == '\n') {
                    if (write_row < WRITE_ROWS-1) {
                        write_row++; write_col = 0; write_lines[write_row][0] = 0;
                    }
                } else if (key == '\b') {
                    if (write_col > 0) {
                        write_col--; write_lines[write_row][write_col] = 0;
                    } else if (write_row > 0) {
                        write_row--; write_col = (int)kstrlen(write_lines[write_row]);
                    }
                } else if (key >= 32 && (uint8_t)key < 127 && write_col < WRITE_COLS-1) {
                    write_lines[write_row][write_col++] = key;
                    write_lines[write_row][write_col]   = 0;
                }
            } else if (top == WIN_SNAKE) {
                if (snake_over && (key=='r' || key=='R')) snake_reset();
                else if (key == KEY_UP    && snake_dy==0) { snake_dx=0;  snake_dy=-1; }
                else if (key == KEY_DOWN  && snake_dy==0) { snake_dx=0;  snake_dy=1;  }
                else if (key == KEY_LEFT  && snake_dx==0) { snake_dx=-1; snake_dy=0;  }
                else if (key == KEY_RIGHT && snake_dx==0) { snake_dx=1;  snake_dy=0;  }
            } else if (top == WIN_2048) {
                if (g2048_over && (key=='r' || key=='R')) g2048_reset();
                else if (!g2048_over && key == KEY_LEFT)  g2048_move(0);
                else if (!g2048_over && key == KEY_RIGHT) g2048_move(1);
                else if (!g2048_over && key == KEY_UP)    g2048_move(2);
                else if (!g2048_over && key == KEY_DOWN)  g2048_move(3);
            } else if (top == WIN_MINE) {
                if (key=='r' || key=='R') mine_reset();
            } else if (top == WIN_CALC) {
                char kb[2] = { key, 0 };
                if (key>='0' && key<='9') calc_press(kb);
                else if (key=='+'||key=='-'||key=='*'||key=='/') calc_press(kb);
                else if (key=='\n' || key=='=') calc_press("=");
                else if (key=='c' || key=='C' || key=='\b') calc_press("C");
            }
        }

        int right_now = m->right || (latched & 2);
        if (right_now && !prev_right && !ctxmenu_open && !launcher_open &&
            confirm_pending == CONFIRM_NONE && my >= 10) {
            int hitw = -1;
            for (int i=zcount-1;i>=0 && hitw<0;i--) {
                int t = zorder[i]; winrec_t *w = &wins[t];
                if (w->active && !w->minimized && point_in(mx,my,w->x,w->y,w->w,w->h)) hitw = t;
            }
            if (hitw < 0) { ctxmenu_open = 1; ctxmenu_x = mx; ctxmenu_y = my; }
            else if (hitw == WIN_MINE) mine_handle_rightclick(&wins[hitw], mx, my);
        }
        prev_right = right_now;

        int left_now = m->left || (latched & 1);
        if (left_now && !prev_left && confirm_pending != CONFIRM_NONE) {
            confirm_handle_click(mx, my);
        } else if (left_now && !prev_left && ctxmenu_open) {
            int row = ctxmenu_hit(mx, my);
            ctxmenu_open = 0;
            if (row == 0) wm_open_new_terminal();
            else if (row == 1) wm_cascade();
            else if (row == 2) wm_tile();
            else if (row == 3) wm_close_all();
            else if (row == 4) confirm_pending = CONFIRM_REBOOT;
            else if (row == 5) confirm_pending = CONFIRM_POWEROFF;
        } else if (left_now && !prev_left && launcher_open) {
            launch_item_t items[LAUNCH_MAX];
            int n = launcher_build(items, LAUNCH_MAX);
            int row = launcher_hit(mx, my, n);
            launcher_open = 0;
            if (row >= 0) {
                if (items[row].is_pkg) launcher_run_pkg(items[row].pkg_idx);
                else launcher_open_wintype(items[row].wintype);
            }
        } else if (left_now && !prev_left) {
            int hit = -1;
            for (int i=zcount-1;i>=0 && hit<0;i--) {
                int t = zorder[i];
                winrec_t *w = &wins[t];
                if (w->active && !w->minimized && point_in(mx,my,w->x,w->y,w->w,w->h)) hit = t;
            }

            if (hit >= 0) {
                winrec_t *w = &wins[hit];
                wm_push_front(hit);
                if (my < w->y+11) {
                    if (mx>=w->x+w->w-11 && mx<w->x+w->w-2) {
                        wm_close(hit);
                    } else if (mx>=w->x+w->w-22 && mx<w->x+w->w-13) {
                        w->minimized = 1;
                    } else if (hit == last_tb_win && timer_ticks()-last_tb_tick < 40) {
                        wm_toggle_maximize(hit);
                        last_tb_win = -1;
                    } else {
                        dragging = hit; drag_ox = mx-w->x; drag_oy = my-w->y;
                        last_tb_win = hit; last_tb_tick = timer_ticks();
                    }
                } else if (mx>=w->x+w->w-8 && mx<w->x+w->w && my>=w->y+w->h-8 && my<w->y+w->h) {
                    resizing = hit; resize_ow = w->w; resize_oh = w->h;
                    resize_mx = mx; resize_my = my;
                } else if (hit == WIN_FILES) {
                    files_handle_click(w, mx, my);
                } else if (hit == WIN_SYSMON) {
                    sysmon_handle_click(w, mx, my);
                } else if (hit == WIN_CALC) {
                    calc_handle_click(w, mx, my);
                } else if (hit == WIN_EDITOR) {
                    editor_handle_click(w, mx, my);
                } else if (hit == WIN_PKG) {
                    pkg_handle_click(w, mx, my);
                } else if (hit == WIN_BROWSER) {
                    browser_handle_click(w, mx, my);
                } else if (hit == WIN_WRITE) {
                    write_handle_click(w, mx, my);
                } else if (hit == WIN_PAINT) {
                    paint_handle_click(w, mx, my);
                } else if (hit == WIN_PIANO) {
                    piano_handle_click(w, mx, my);
                } else if (hit == WIN_MINE) {
                    mine_handle_click(w, mx, my);
                } else if (hit == WIN_SETTINGS) {
                    settings_handle_click(w, mx, my);
                }
            } else if (my < 10 && mx < 48) {
                launcher_open = 1;
            } else if (my < 10 && mx >= tb_cascade_x0 && mx < tb_cascade_x1) {
                wm_cascade();
            } else if (my < 10 && mx >= tb_tile_x0 && mx < tb_tile_x1) {
                wm_tile();
            } else if (my < 10) {
                int th = taskbar_hit(mx, my);
                if (th >= 0) { wins[th].minimized = 0; wm_push_front(th); }
            } else if (mx < 48 && my >= ICON_START_Y) {
                int iconidx = (my-ICON_START_Y)/ICON_SLOT;
                if (iconidx==0)      wm_open(WIN_TERMINAL, 40,  20, TERM_W, TERM_H);
                else if (iconidx==1) wm_open(WIN_FILES,   760,  20, 230, 290);
                else if (iconidx==2) wm_open(WIN_SYSMON,  760, 330, 230, 290);
                else if (iconidx==3) wm_open(WIN_CALC,    480, 500, 190, 230);
                else if (iconidx==4) wm_open(WIN_EDITOR,   40, 500, 420, 230);
                else if (iconidx==5) wm_open(WIN_PKG,     690, 500, 300, 230);
                else if (iconidx==6) wm_open(WIN_BROWSER, 260,  60, 500, 420);
                else if (iconidx==7) wm_open(WIN_WRITE,   300, 100, 520, 400);
                else if (iconidx==8) wm_open(WIN_PAINT,   340,  80, 340, 260);
                else if (iconidx==9) wm_open(WIN_SNAKE,   400, 200, 220, 216);
                else if (iconidx==10) wm_open(WIN_PIANO,  420, 300, 240, 150);
                else if (iconidx==11) wm_open(WIN_2048,   460, 150, 220, 260);
                else if (iconidx==12) wm_open(WIN_MINE,   430, 140, 160, 190);
                else if (iconidx==13) wm_open(WIN_SETTINGS, 420, 220, 260, 190);
                else if (iconidx==14) running = 0;
            }
        }

        if (left_now && dragging >= 0) {
            winrec_t *w = &wins[dragging];
            int nx = mx-drag_ox, ny = my-drag_oy;
            if (nx < 0) nx = 0;
            if (ny < 10) ny = 10;
            if (nx+w->w > GUI_WIDTH)  nx = GUI_WIDTH-w->w;
            if (ny+w->h > GUI_HEIGHT) ny = GUI_HEIGHT-w->h;
            w->x = nx; w->y = ny;
        } else if (!left_now && dragging >= 0) {
            winrec_t *w = &wins[dragging];
            if (mx <= 1) {
                w->x = 0; w->y = 10; w->w = GUI_WIDTH/2; w->h = GUI_HEIGHT-10;
            } else if (mx >= GUI_WIDTH-2) {
                w->x = GUI_WIDTH/2; w->y = 10; w->w = GUI_WIDTH/2; w->h = GUI_HEIGHT-10;
            }
        }
        if (!left_now) dragging = -1;

        if (left_now && resizing >= 0) {
            winrec_t *w = &wins[resizing];
            int nw = resize_ow + (mx - resize_mx);
            int nh = resize_oh + (my - resize_my);
            if (nw < w->min_w) nw = w->min_w;
            if (nh < w->min_h) nh = w->min_h;
            if (w->x+nw > GUI_WIDTH)  nw = GUI_WIDTH  - w->x;
            if (w->y+nh > GUI_HEIGHT) nh = GUI_HEIGHT - w->y;
            w->w = nw; w->h = nh;
        }
        if (!left_now) resizing = -1;

        if (left_now && g_painting && wins[WIN_PAINT].active) {
            winrec_t *w = &wins[WIN_PAINT];
            int cx0 = w->x+4, cy0 = w->y+14;
            int lx = mx-cx0, ly = my-cy0;
            if (lx >= 0 && lx < PAINT_W && ly >= 0 && ly < PAINT_H)
                paint_dot(lx, ly);
        }
        if (!left_now) g_painting = 0;

        if (!left_now && piano_active >= 0) {
            speaker_off();
            piano_active = -1;
        }

        if (gui_beep_off_at && timer_ticks() >= gui_beep_off_at) {
            speaker_off();
            gui_beep_off_at = 0;
        }

        prev_left = left_now;

        __asm__ volatile("hlt");
    }

    for (int i = 0; i < VGA_TTYS; i++) term_close_session(i);
    gui_active = 0;
    gui_exit();
}
