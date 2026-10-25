#include "serial.h"
#include "vga.h"
#include "kstring.h"
#include <stdint.h>

#define UART_DATA     0
#define UART_IER      1
#define UART_IIR      2
#define UART_FCR      2
#define UART_LCR      3
#define UART_MCR      4
#define UART_LSR      5
#define UART_MSR      6
#define UART_SCRATCH  7

#define LCR_8BIT      0x03
#define LCR_1STOP     0x00
#define LCR_NO_PARITY 0x00
#define LCR_DLAB      0x80

#define LSR_DR        0x01
#define LSR_THRE      0x20

#define FCR_ENABLE    0x01
#define FCR_CLR_RX    0x02
#define FCR_CLR_TX    0x04
#define FCR_TRIG_14   0xC0

static int com1_ready = 0;

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

int serial_init(uint16_t port) {
    outb(port + UART_IER, 0x00);

    outb(port + UART_LCR, LCR_DLAB);
    outb(port + UART_DATA, 0x01);
    outb(port + UART_IER,  0x00);

    outb(port + UART_LCR, LCR_8BIT | LCR_1STOP | LCR_NO_PARITY);

    outb(port + UART_FCR, FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX | FCR_TRIG_14);

    outb(port + UART_MCR, 0x0B);

    outb(port + UART_MCR, 0x1E);
    outb(port + UART_DATA, 0xAE);

    for (int i = 0; i < 10000; i++) {
        if (inb(port + UART_LSR) & LSR_DR) break;
    }

    if (inb(port + UART_DATA) != 0xAE) {
        outb(port + UART_MCR, 0x0F);
        return -1;
    }

    outb(port + UART_MCR, 0x0F);

    if (port == COM1) com1_ready = 1;
    return 0;
}

void serial_putchar(uint16_t port, char c) {
    int timeout = 100000;
    while (!(inb(port + UART_LSR) & LSR_THRE) && --timeout);

    if (c == '\n') {
        outb(port + UART_DATA, '\r');

        timeout = 100000;
        while (!(inb(port + UART_LSR) & LSR_THRE) && --timeout);
    }
    outb(port + UART_DATA, (uint8_t)c);
}

void serial_puts(uint16_t port, const char *s) {
    while (*s) serial_putchar(port, *s++);
}

void serial_write(uint16_t port, const char *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) serial_putchar(port, buf[i]);
}

int serial_has_data(uint16_t port) {
    return (inb(port + UART_LSR) & LSR_DR) ? 1 : 0;
}

char serial_getchar(uint16_t port) {
    if (!serial_has_data(port)) return 0;
    return (char)inb(port + UART_DATA);
}

int serial_ready(void) { return com1_ready; }

static void serial_put_dec(uint16_t port, uint32_t n) {
    char buf[12]; int i = 10; buf[11] = 0;
    if (!n) { serial_putchar(port, '0'); return; }
    while (n) { buf[i--] = '0' + n%10; n/=10; }
    serial_puts(port, buf+i+1);
}

static void serial_put_hex(uint16_t port, uint32_t n) {
    const char *h = "0123456789ABCDEF";
    serial_puts(port, "0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putchar(port, h[(n>>i)&0xF]);
}

void serial_printf(const char *fmt, ...) {
    if (!com1_ready) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if      (*fmt=='s') { char *s=__builtin_va_arg(ap,char*); serial_puts(COM1,s?s:"(null)"); }
            else if (*fmt=='d') { int d=__builtin_va_arg(ap,int); if(d<0){serial_putchar(COM1,'-');d=-d;} serial_put_dec(COM1,(uint32_t)d); }
            else if (*fmt=='u') { serial_put_dec(COM1,__builtin_va_arg(ap,uint32_t)); }
            else if (*fmt=='x') { serial_put_hex(COM1,__builtin_va_arg(ap,uint32_t)); }
            else if (*fmt=='c') { serial_putchar(COM1,(char)__builtin_va_arg(ap,int)); }
            else if (*fmt=='%') { serial_putchar(COM1,'%'); }
        } else {
            serial_putchar(COM1, *fmt);
        }
        fmt++;
    }
    __builtin_va_end(ap);
}

void klog(const char *fmt, ...) {
    char buf[512];
    int  pos = 0;

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    while (*fmt && pos < 510) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                char *s = __builtin_va_arg(ap, char*);
                if (!s) s = "(null)";
                while (*s && pos < 510) buf[pos++] = *s++;
            } else if (*fmt == 'd') {
                int d = __builtin_va_arg(ap, int);
                if (d < 0) { buf[pos++] = '-'; d = -d; }
                char tmp[12]; int i = 10; tmp[11] = 0;
                if (!d) { buf[pos++] = '0'; }
                else { uint32_t u=(uint32_t)d; while(u){tmp[i--]='0'+u%10;u/=10;} while(++i<=10&&pos<510)buf[pos++]=tmp[i]; }
            } else if (*fmt == 'u') {
                uint32_t u = __builtin_va_arg(ap, uint32_t);
                char tmp[12]; int i = 10; tmp[11] = 0;
                if (!u) { buf[pos++] = '0'; }
                else { while(u){tmp[i--]='0'+u%10;u/=10;} while(++i<=10&&pos<510)buf[pos++]=tmp[i]; }
            } else if (*fmt == 'x') {
                uint32_t v = __builtin_va_arg(ap, uint32_t);
                const char *h="0123456789ABCDEF";
                if (pos+10<510) {
                    buf[pos++]='0'; buf[pos++]='x';
                    for(int i=28;i>=0;i-=4) buf[pos++]=h[(v>>i)&0xF];
                }
            } else if (*fmt == 'c') {
                buf[pos++] = (char)__builtin_va_arg(ap, int);
            } else if (*fmt == '%') {
                buf[pos++] = '%';
            }
        } else {
            buf[pos++] = *fmt;
        }
        fmt++;
    }
    buf[pos] = 0;
    __builtin_va_end(ap);

    vga_puts(buf);

    if (com1_ready)
        serial_write(COM1, buf, (uint32_t)pos);
}

void serial_hexdump(uint16_t port, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    const char *h = "0123456789ABCDEF";
    char line[80];

    for (uint32_t off = 0; off < len; off += 16) {
        int i = 0;
        uint32_t a = off;
        line[i++]='0'; line[i++]='x';
        for (int s=28;s>=0;s-=4) line[i++]=h[(a>>s)&0xF];
        line[i++]=':'; line[i++]=' ';

        for (int j = 0; j < 16; j++) {
            if (off+j < len) {
                line[i++] = h[p[off+j]>>4];
                line[i++] = h[p[off+j]&0xF];
            } else {
                line[i++] = ' '; line[i++] = ' ';
            }
            line[i++] = ' ';
            if (j==7) line[i++] = ' ';
        }
        line[i++] = ' '; line[i++] = '|';

        for (int j = 0; j < 16 && off+j < len; j++) {
            uint8_t c = p[off+j];
            line[i++] = (c>=32&&c<127) ? (char)c : '.';
        }
        line[i++] = '|'; line[i++] = '\n'; line[i] = 0;
        serial_write(port, line, (uint32_t)i);
    }
}
