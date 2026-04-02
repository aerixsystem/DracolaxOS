/* kernel/limits.h — System resource limit enforcement
 *
 * Memory rule: deny any allocation that would push usage above 90%.
 * The remaining 10% is reserved for the kernel/system.
 * When usage hits 80%, a swap zone in /storage/ramdisk is used.
 */
#ifndef LIMITS_H
#define LIMITS_H

#include "types.h"

#define MEM_WARN_PCT      80    /* % at which we activate swap-like eviction  */
#define MEM_DENY_PCT      90    /* % at which new allocs are blocked           */
#define VOL_MIN_PCT        0
#define VOL_MAX_PCT      100    /* warn if > 100%, hard deny at 200%           */
#define VOL_WARN_PCT     100
#define BRIGHTNESS_MAX   100

typedef struct {
    uint32_t mem_total_kb;
    uint32_t mem_used_kb;
    uint32_t mem_warn_threshold_kb;
    uint32_t mem_deny_threshold_kb;
    uint8_t  mem_warn_active;     /* 1 if in warn zone    */
    uint8_t  mem_deny_active;     /* 1 if in deny zone    */
    uint8_t  swap_active;         /* 1 if swap is active  */
    uint32_t volume_pct;          /* current volume 0-200 */
    uint32_t brightness_pct;      /* current brightness   */
} system_limits_t;

extern system_limits_t g_limits;

/* Initialise limits subsystem */
void limits_init(void);

/* Update memory stats and enforce thresholds — call periodically */
void limits_update(void);

/* Check if a new kmalloc of `size` bytes should be allowed */
int  limits_allow_alloc(size_t size);

/* Check if volume level is valid; returns 0 if denied */
int  limits_set_volume(uint32_t pct);

/* Get current volume */
uint32_t limits_get_volume(void);

/* Set brightness */
int  limits_set_brightness(uint32_t pct);

/* Print limits summary to VGA */
void limits_print_status(void);

#endif /* LIMITS_H */
