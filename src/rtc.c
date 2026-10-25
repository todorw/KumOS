#include "rtc.h"
#include "vga.h"
#include <stdint.h>

#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

#define RTC_SEC    0x00
#define RTC_MIN    0x02
#define RTC_HOUR   0x04
#define RTC_DAY    0x07
#define RTC_MONTH  0x08
#define RTC_YEAR   0x09
#define RTC_REGA   0x0A
#define RTC_REGB   0x0B

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

static void rtc_wait(void) {
    do {
        outb(CMOS_ADDR, RTC_REGA);
    } while (inb(CMOS_DATA) & 0x80);
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

void rtc_init(void) {
}

rtc_time_t rtc_read(void) {
    rtc_time_t t;
    rtc_wait();

    uint8_t sec   = cmos_read(RTC_SEC);
    uint8_t min   = cmos_read(RTC_MIN);
    uint8_t hour  = cmos_read(RTC_HOUR);
    uint8_t day   = cmos_read(RTC_DAY);
    uint8_t month = cmos_read(RTC_MONTH);
    uint8_t year  = cmos_read(RTC_YEAR);
    uint8_t regb  = cmos_read(RTC_REGB);

    if (!(regb & 0x04)) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
    }

    if (!(regb & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    t.second = sec;
    t.minute = min;
    t.hour   = hour;
    t.day    = day;
    t.month  = month;

    t.year   = 2000 + year;

    return t;
}

static void print2(uint8_t n) {
    vga_putchar('0' + n/10);
    vga_putchar('0' + n%10);
}

void rtc_print(void) {
    rtc_time_t t = rtc_read();
    vga_put_dec(t.year); vga_putchar('-');
    print2(t.month);     vga_putchar('-');
    print2(t.day);       vga_puts("  ");
    print2(t.hour);      vga_putchar(':');
    print2(t.minute);    vga_putchar(':');
    print2(t.second);
}

uint32_t rtc_timestamp(void) {
    rtc_time_t t = rtc_read();
    uint32_t days = (uint32_t)(t.year - 2000) * 365
                  + (uint32_t)(t.month - 1) * 30
                  + t.day;
    return days * 86400
         + (uint32_t)t.hour   * 3600
         + (uint32_t)t.minute * 60
         + t.second;
}
