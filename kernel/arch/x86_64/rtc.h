/* kernel/rtc.h — x86 CMOS Real-Time Clock driver */
#ifndef RTC_H
#define RTC_H
#include "../../types.h"

typedef struct {
    uint8_t  sec, min, hour;
    uint8_t  day, month;
    uint16_t year;
} rtc_time_t;

void rtc_init(void);
void rtc_read(rtc_time_t *t);

/* Format into ISO string: "YYYY-MM-DD_HH-MM-SS" (20 chars + NUL) */
void rtc_format(const rtc_time_t *t, char *buf, size_t sz);

/* Monotonic tick count (seconds since boot, from PIT) */
uint64_t rtc_uptime_secs(void);

#endif
