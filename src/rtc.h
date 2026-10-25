#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
    uint8_t  second;
    uint8_t  minute;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
} rtc_time_t;

void      rtc_init(void);
rtc_time_t rtc_read(void);
void      rtc_print(void);

uint32_t  rtc_timestamp(void);

#endif
