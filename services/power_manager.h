#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H
#include "../kernel/types.h"

typedef struct {
    uint32_t brightness;
    uint8_t  on_battery;
    uint8_t  battery_pct;
} power_state_t;

void power_manager_init(void);
power_state_t *power_get_state(void);
void power_set_brightness(uint32_t pct);
void power_manager_task(void);

/* Direct power actions — callable from shell, desktop, kernel panic */
void power_shutdown(void);   /* ACPI S5 soft-off                  */
void power_reboot(void);     /* keyboard-controller pulse + triple */
#endif