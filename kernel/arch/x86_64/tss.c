/* kernel/tss.c — 64-bit GDT rebuild + TSS setup
 *
 * Replaces the minimal boot.s GDT (null/kernel-code/kernel-data) with a
 * full 8-entry GDT that adds user-space segments and a TSS descriptor,
 * enabling ring-3 execution.
 *
 * GDT slots:
 *   [0] 0x00 — null
 *   [1] 0x08 — 64-bit kernel code  (DPL=0, L=1)
 *   [2] 0x10 — 64-bit kernel data  (DPL=0)
 *   [3] 0x18 — 64-bit user data    (DPL=3)   ← new
 *   [4] 0x20 — 64-bit user code    (DPL=3, L=1) ← new
 *   [5] 0x28 — TSS low  (16 bytes) ← new
 *   [6] 0x30 — TSS high (upper 8 bytes of 64-bit TSS descriptor) ← new
 *
 * References:
 *   Intel SDM Vol.3 §3.4.5 — segment descriptors
 *   Intel SDM Vol.3 §8.7   — TSS in 64-bit mode
 *   AMD64 APM Vol.2 §2.4   — SYSCALL/SYSRET
 */
#include "tss.h"
#include "../../types.h"
#include "../../log.h"
#include "../../klibc.h"

/* ---- Global TSS ---------------------------------------------------------- */

static tss64_t g_tss;

/* 512-byte kernel stack used as RSP0 during ring-3 → ring-0 transitions.
 * Separate from task stacks so a user fault doesn't corrupt task state. */
static uint8_t g_tss_kstack[4096] __attribute__((aligned(16)));

tss64_t *tss_get(void) { return &g_tss; }

/* ---- GDT ----------------------------------------------------------------- */

/* Each GDT entry is 8 bytes.  The TSS descriptor is 16 bytes (2 slots). */
typedef uint64_t gdt_entry_t;

static gdt_entry_t g_gdt[7];   /* 7 slots: null + 4 segments + 2 TSS words */

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

static gdtr_t g_gdtr;

/* Build a flat 64-bit code/data segment descriptor.
 *   dpl  — 0 for kernel, 3 for user
 *   code — 1 for code segment, 0 for data segment
 */
static uint64_t make_seg(int dpl, int code) {
    /* P=1 S=1 L=1(code) G=1
     * Bit fields (Intel Vol.3 Table 3-1):
     *   [15]    P      = 1 (present)
     *   [14:13] DPL    = dpl
     *   [12]    S      = 1 (code/data, not system)
     *   [11:8]  type   = 0xA (exec/read) for code, 0x2 (read/write) for data
     *   [53]    L      = 1 for 64-bit code (must be 0 for data in 64-bit mode)
     *   [55]    G      = 1 (granularity = 4KB pages)
     * Limit and base are ignored in 64-bit mode for code/data segments.
     */
    uint64_t type  = code ? 0xAULL : 0x2ULL;  /* exec/read : read/write */
    uint64_t desc  = 0;
    desc |= 0x0000FFFFULL;                     /* limit low  [15:0]  = 0xFFFF */
    desc |= (uint64_t)dpl   << 45;             /* DPL        [46:45] */
    desc |= 1ULL             << 44;            /* S=1        [44]    */
    desc |= type             << 40;            /* type       [43:40] */
    desc |= 1ULL             << 47;            /* P=1        [47]    */
    desc |= 0xFULL           << 48;            /* limit high [51:48] = 0xF */
    if (code) desc |= 1ULL   << 53;            /* L=1        [53] code only */
    desc |= 1ULL             << 55;            /* G=1        [55]    */
    return desc;
}

/* Build the TSS descriptor pair (16 bytes total).
 * In 64-bit mode a TSS descriptor is 16 bytes: the standard 8-byte
 * descriptor encoding base[31:0] + limit + flags, plus an extra 8-byte
 * extension carrying base[63:32].
 */
static void make_tss_desc(gdt_entry_t *lo, gdt_entry_t *hi, uint64_t base, uint32_t limit) {
    /* Low 8 bytes */
    *lo  = (uint64_t)(limit & 0xFFFFULL);               /* limit[15:0]  */
    *lo |= (base  & 0xFFFFFFULL) << 16;                 /* base[23:0]   */
    *lo |= 0x89ULL               << 40;                 /* P=1 type=9 (64-bit TSS avail) */
    *lo |= ((uint64_t)(limit >> 16) & 0xFULL) << 48;   /* limit[19:16] */
    *lo |= ((base >> 24) & 0xFFULL) << 56;              /* base[31:24]  */
    /* High 8 bytes — upper 32 bits of base, rest reserved */
    *hi  = (base >> 32) & 0xFFFFFFFFULL;
}

/* ---- Public API ---------------------------------------------------------- */

void tss_init(void) {
    /* Zero TSS, set iomap_base past the end so all I/O is permitted in
     * ring 0 and denied in ring 3 via the absent I/O bitmap. */
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.iomap_base = (uint16_t)sizeof(tss64_t);

    /* Point RSP0 at the top of the kernel fault stack */
    g_tss.rsp0 = (uint64_t)(uintptr_t)(g_tss_kstack + sizeof(g_tss_kstack));

    /* Build GDT */
    g_gdt[0] = 0;                              /* null */
    g_gdt[1] = make_seg(0, 1);                /* kernel code  0x08 */
    g_gdt[2] = make_seg(0, 0);                /* kernel data  0x10 */
    g_gdt[3] = make_seg(3, 0);                /* user data    0x18 */
    g_gdt[4] = make_seg(3, 1);                /* user code    0x20 */
    make_tss_desc(&g_gdt[5], &g_gdt[6],
                  (uint64_t)(uintptr_t)&g_tss,
                  (uint32_t)(sizeof(tss64_t) - 1));

    /* Load GDTR */
    g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdtr.base  = (uint64_t)(uintptr_t)g_gdt;
    __asm__ volatile ("lgdt %0" :: "m"(g_gdtr));

    /* Reload segment registers with new selectors.
     * CS cannot be moved directly; use a far-return trick. */
    __asm__ volatile (
        "push $0x08\n"          /* new CS = kernel code */
        "lea  1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"     /* kernel data */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "xor %%ax, %%ax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        ::: "rax", "memory"
    );

    /* Load TSS */
    __asm__ volatile ("ltr %0" :: "r"((uint16_t)GDT_TSS_SEL));

    kinfo("TSS: GDT rebuilt (8 entries), TSS loaded at 0x%llx, RSP0=0x%llx\n",
          (unsigned long long)(uintptr_t)&g_tss,
          (unsigned long long)g_tss.rsp0);
}

void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}
