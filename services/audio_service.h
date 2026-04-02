#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H
#include "../kernel/types.h"
typedef struct { uint32_t volume; uint8_t muted; uint32_t sample_rate; } audio_state_t;
void     audio_service_init(void);
void     audio_set_volume(uint32_t vol);
uint32_t audio_get_volume(void);
void     audio_mute(int on);
int      audio_is_muted(void);
void     audio_service_task(void);

/* PC speaker beep — always available on x86, no driver needed */
void     audio_beep(uint32_t freq_hz, uint32_t duration_ms);
#endif
