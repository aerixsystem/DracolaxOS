#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H
#include "../../types.h"
typedef struct audio_driver {
    const char *name;
    int  (*init)(void);
    int  (*write)(const int16_t *samples, uint32_t count);
    void (*set_volume)(uint8_t vol);
    void (*shutdown)(void);
} audio_driver_t;
void audio_driver_register(audio_driver_t *d);
#endif
