/* kernel/rtc.c — CMOS RTC driver
 *
 * Reads CMOS registers via I/O ports 0x70 (index) and 0x71 (data).
 * BCD → binary conversion applied unless bit 2 of status register B is set.
 * Waits for update-in-progress flag to clear before reading.
 *
 * Reference: https://wiki.osdev.org/CMOS#Reading_All_RTC_Time_and_Date_Registers
 */
#include "../../types.h"
#include "rtc.h"
#include "../../klibc.h"
#include "../../log.h"
#include "pic.h"

#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71
#define RTC_UIP    0x80   /* Update-In-Progress bit in status reg A */

static inline uint8_t cmos_read(uint8_t reg) {
    uint8_t v;
    __asm__ volatile("outb %0, %1" :: "a"(reg),  "Nd"((uint16_t)CMOS_ADDR));
    __asm__ volatile("inb %1, %0"  : "=a"(v)  : "Nd"((uint16_t)CMOS_DATA));
    return v;
}

static inline void wait_no_uip(void) {
    /* Spin until UIP clears — typically < 1 ms */
    int i = 100000;
    while (--i && (cmos_read(0x0A) & RTC_UIP));
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

void rtc_init(void) {
    kinfo("RTC: CMOS real-time clock driver initialised\n");
}

void rtc_read(rtc_time_t *t) {
    wait_no_uip();
    uint8_t sb = cmos_read(0x0B);
    int bcd = !(sb & 0x04);

    t->sec   = cmos_read(0x00);
    t->min   = cmos_read(0x02);
    t->hour  = cmos_read(0x04);
    t->day   = cmos_read(0x07);
    t->month = cmos_read(0x08);
    uint8_t y = cmos_read(0x09);
    uint8_t c = cmos_read(0x32);   /* century register */

    if (bcd) {
        t->sec   = bcd2bin(t->sec);
        t->min   = bcd2bin(t->min);
        t->hour  = bcd2bin(t->hour);
        t->day   = bcd2bin(t->day);
        t->month = bcd2bin(t->month);
        y        = bcd2bin(y);
        c        = bcd2bin(c);
    }
    t->year = (uint16_t)((c ? c : 20) * 100 + y);
}

void rtc_format(const rtc_time_t *t, char *buf, size_t sz) {
    snprintf(buf, sz, "%04u-%02u-%02u_%02u-%02u-%02u",
             t->year, t->month, t->day,
             t->hour, t->min, t->sec);
}

uint64_t rtc_uptime_secs(void) {
    return (uint64_t)pit_ticks() / 100;   /* PIT at 100 Hz */
}
