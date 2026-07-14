/* services/audio_service.c — DracolaxOS audio service */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
#include "audio_service.h"

static audio_state_t g_audio = { .volume = 80, .muted = 0, .sample_rate = 44100 };

void audio_service_init(void) {
    kinfo("AUDIO: service init, vol=%u sr=%u\n",
          g_audio.volume, g_audio.sample_rate);
}

void audio_set_volume(uint32_t vol) {
    if (vol > 100) vol = 100;
    g_audio.volume = vol;
}
uint32_t audio_get_volume(void) { return g_audio.volume; }
void audio_mute(int on)         { g_audio.muted = (uint8_t)on; }
int  audio_is_muted(void)       { return (int)g_audio.muted; }

/* ---- PC Speaker beep via PIT channel 2 ----------------------------------
 * The PC speaker (port 0x61) is always available on x86 hardware and VMs.
 * Frequency: PIT channel 2 input = 1193182 Hz.
 * divisor = 1193182 / freq_hz.
 * This gives DracolaxOS *some* real audio output without a full AC97 driver.
 * -------------------------------------------------------------------- */
static inline void _outb_a(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t _inb_a(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v;
}

void audio_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) return;
    uint32_t divisor = 1193182u / freq_hz;
    /* Program PIT channel 2 (speaker), mode 3 (square wave) */
    _outb_a(0x43, 0xB6);                            /* channel 2, mode 3 */
    _outb_a(0x42, (uint8_t)(divisor & 0xFF));        /* low byte  */
    _outb_a(0x42, (uint8_t)((divisor >> 8) & 0xFF)); /* high byte */
    /* Enable speaker gate (bits 0+1 of port 0x61) */
    uint8_t prev = _inb_a(0x61);
    _outb_a(0x61, prev | 0x03);
    /* Busy-wait for duration (rough: ~1 ms ≈ 10000 iterations at 1 GHz) */
    for (uint32_t ms = 0; ms < duration_ms; ms++)
        for (volatile int i = 0; i < 8000; i++);
    /* Disable speaker */
    _outb_a(0x61, prev & ~0x03);
}

void audio_service_task(void) {
    audio_service_init();
    kinfo("AUDIO: task running (PC speaker available; AC97/HDA: V2)\n");
    /* Boot chime: two short ascending beeps on PC speaker */
    audio_beep(440,  80);   /* A4 */
    sched_sleep(60);
    audio_beep(880, 80);    /* A5 */
    for (;;) {
        sched_sleep(500);   /* idle — wake if a mix request arrives (V2) */
    }
}
