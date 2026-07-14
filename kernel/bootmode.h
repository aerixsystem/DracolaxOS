/* kernel/bootmode.h — Boot mode selection
 *
 * The GRUB menu entry passes a cmdline arg via multiboot2:
 *   "mode=graphical"  → full GUI + desktop + atlas animation
 *   "mode=text"       → shell only, no desktop, no animation
 *   "mode=shell"      → minimal shell, no security init, no atlas
 *   (nothing)         → auto: graphical if FB available, else text
 *
 * The mode is parsed from MB2 tag type 1 (cmdline) in kmain before
 * anything else, and stored in g_boot_mode.
 */
#ifndef BOOTMODE_H
#define BOOTMODE_H

#include "types.h"

#define BOOT_MODE_AUTO       0   /* detect: graphical if FB, else text */
#define BOOT_MODE_GRAPHICAL  1   /* full desktop + compositor + atlas   */
#define BOOT_MODE_TEXT       2   /* shell only, no compositor           */
#define BOOT_MODE_SHELL      3   /* bare shell, minimal init            */

extern uint8_t g_boot_mode;

/* Parse MB2 cmdline tag to set g_boot_mode.
 * Must be called with mbi_addr from kmain, before fb_init. */
void bootmode_init(uint64_t mbi_addr);

/* Returns 1 if this mode should run the full desktop */
static inline int bootmode_wants_desktop(void) {
    return g_boot_mode == BOOT_MODE_GRAPHICAL ||
           g_boot_mode == BOOT_MODE_AUTO;
}

/* Returns 1 if this mode should play the atlas animation */
static inline int bootmode_wants_atlas(void) {
    return g_boot_mode == BOOT_MODE_GRAPHICAL ||
           g_boot_mode == BOOT_MODE_AUTO;
}

/* Returns 1 if this mode needs full security + storage init */
static inline int bootmode_wants_full_init(void) {
    return g_boot_mode != BOOT_MODE_SHELL;
}

#endif /* BOOTMODE_H */
