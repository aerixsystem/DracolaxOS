/* kernel/bootmode.c */
#include "types.h"
#include "klibc.h"
#include "log.h"
#include "bootmode.h"

uint8_t g_boot_mode = BOOT_MODE_AUTO;

/* MB2 tag type 1 = cmdline: uint32 type, uint32 size, then null-terminated string */
void bootmode_init(uint64_t mbi_addr) {
    if (!mbi_addr) return;

    uint32_t total = *(uint32_t *)mbi_addr;
    uint8_t *p     = (uint8_t *)(mbi_addr + 8);
    uint8_t *end   = (uint8_t *)mbi_addr + total;

    while (p < end) {
        uint32_t tag_type = *(uint32_t *)p;
        uint32_t tag_size = *(uint32_t *)(p + 4);
        if (tag_size < 8) break;
        if (tag_type == 0) break; /* end tag */

        if (tag_type == 1 && tag_size > 8) {
            /* cmdline string starts at p+8 */
            const char *cmdline = (const char *)(p + 8);
            kdebug("BOOTMODE: cmdline = '%s'\n", cmdline);

            /* Look for mode=xxx */
            const char *m = cmdline;
            while (*m) {
                if (strncmp(m, "mode=", 5) == 0) {
                    const char *val = m + 5;
                    if (strncmp(val, "graphical", 9) == 0) {
                        g_boot_mode = BOOT_MODE_GRAPHICAL;
                    } else if (strncmp(val, "text", 4) == 0) {
                        g_boot_mode = BOOT_MODE_TEXT;
                    } else if (strncmp(val, "shell", 5) == 0) {
                        g_boot_mode = BOOT_MODE_SHELL;
                    }
                    break;
                }
                m++;
            }
            break;
        }

        uint32_t next = (tag_size + 7u) & ~7u;
        if (next == 0) break;
        p += next;
    }

    static const char *names[] = {"auto","graphical","text","shell"};
    kinfo("BOOTMODE: mode=%s (%u)\n",
          g_boot_mode <= 3 ? names[g_boot_mode] : "?", g_boot_mode);
}
