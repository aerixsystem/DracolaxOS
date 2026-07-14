#ifndef INPUT_DRIVER_H
#define INPUT_DRIVER_H
#include "../../types.h"
typedef enum {INPUT_EV_KEY=1,INPUT_EV_MOUSE=2,INPUT_EV_TOUCH=3} input_ev_type_t;
typedef struct {
    input_ev_type_t type;
    union {
        struct{uint16_t keycode;uint8_t pressed;uint8_t mods;}key;
        struct{int16_t dx,dy;uint8_t buttons;}mouse;
        struct{int32_t x,y;uint8_t contact;}touch;
    };
} input_event_t;
void input_push(const input_event_t *ev);
int  input_poll(input_event_t *out);
#endif
