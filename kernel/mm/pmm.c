/* kernel/pmm.c — Bitmap physical memory allocator
 *
 * Design:
 *   One bit per 4KB page.  0 = free, 1 = used.
 *   Bitmap stored at the first aligned address after the kernel image.
 *   Supports up to 4GB (1M pages, 128KB bitmap).
 *
 * API: pmm_alloc() → first-fit scan, O(n/32) per call.
 */
#include "../types.h"
#include "pmm.h"
#include "paging.h"
#include "../klibc.h"
#include "../log.h"

/* Multiboot2 tag types */
#define MB2_TAG_END   0
#define MB2_TAG_MMAP  6
#define MB2_MMAP_AVAIL 1

struct mb2_tag      { uint32_t type, size; };
struct mb2_mmap_tag { uint32_t type, size, entry_sz, entry_ver;
                      uint8_t  entries[]; };
struct mb2_mmap_ent { uint64_t addr, len; uint32_t type, zero; };

/* Bitmap: 1 bit per page.  Max 4GB → 1 048 576 pages → 32 768 uint32_t */
#define MAX_PAGES   (1024u * 1024u)
#define WORDS       (MAX_PAGES / 32u)

static uint32_t bitmap[WORDS];
static uint32_t total_pages;
static uint64_t total_memory;
static uint32_t free_count;

/* ---- bitmap helpers ------------------------------------------------------ */

static inline void set_bit(uint32_t page) {
    bitmap[page / 32] |= (1u << (page % 32));
}
static inline void clr_bit(uint32_t page) {
    bitmap[page / 32] &= ~(1u << (page % 32));
}
static inline int tst_bit(uint32_t page) {
    return (int)((bitmap[page / 32] >> (page % 32)) & 1);
}

/* ---- public API ---------------------------------------------------------- */

void pmm_mark_used(uint64_t addr, uint64_t size) {
    uint32_t start_pg = addr / PAGE_SIZE;
    uint32_t npages   = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < npages; i++) {
        if (start_pg + i < MAX_PAGES && !tst_bit(start_pg + i)) {
            set_bit(start_pg + i);
            if (free_count) free_count--;
        }
    }
}

static void mark_free(uint64_t addr, uint64_t len) {
    uint64_t start = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (addr + len) & ~(uint64_t)(PAGE_SIZE - 1);
    while (start < end && start < (uint64_t)MAX_PAGES * PAGE_SIZE) {
        uint32_t pg = (uint32_t)(start / PAGE_SIZE);
        clr_bit(pg);
        free_count++;
        start += PAGE_SIZE;
    }
}

/* kernel_end symbol from linker script */
extern uint8_t kernel_end[];

void pmm_init(uint64_t mbi_addr) {
    /* Start with everything marked used */
    memset(bitmap, 0xFF, sizeof(bitmap));
    free_count = 0;
    total_pages = 0;
    total_memory = 0;

    if (!mbi_addr) { kinfo("PMM: no MBI — running with 0 free pages\n"); return; }

    struct mb2_tag *tag = (struct mb2_tag *)(uintptr_t)(mbi_addr + 8);

    while (tag->type != MB2_TAG_END) {
        if (tag->type == MB2_TAG_MMAP) {
            struct mb2_mmap_tag *mm = (struct mb2_mmap_tag *)tag;
            uint32_t n = (mm->size - 16) / mm->entry_sz;
            for (uint32_t i = 0; i < n; i++) {
                struct mb2_mmap_ent *e =
                    (struct mb2_mmap_ent *)(mm->entries + i * mm->entry_sz);
                if (e->type == MB2_MMAP_AVAIL) {
                    total_memory += e->len;
                    mark_free(e->addr, e->len);
                }
            }
        }
        uint32_t sz = (tag->size + 7) & ~7u;
        tag = (struct mb2_tag *)((uint8_t *)tag + sz);
    }

    total_pages = (uint32_t)(total_memory / PAGE_SIZE);

    /* Mark first 1MB as used (BIOS, legacy, VGA) */
    pmm_mark_used(0, 0x100000);

    /* Mark kernel image as used */
    uint64_t kend = (uint64_t)(uintptr_t)kernel_end;
    pmm_mark_used(0x100000, kend - 0x100000);

    /* Mark MBI structure itself (assume ≤ 64KB) */
    pmm_mark_used(mbi_addr & ~(PAGE_SIZE - 1), PAGE_SIZE * 16);

    kinfo("PMM: %u MB total, %u pages free\n",
          (uint32_t)(total_memory >> 20), free_count);
}

uint64_t pmm_alloc(void) {
    for (uint32_t w = 0; w < WORDS; w++) {
        if (bitmap[w] == 0xFFFFFFFF) continue; /* all used */
        for (int b = 0; b < 32; b++) {
            if (!((bitmap[w] >> (uint32_t)b) & 1)) {
                uint32_t pg = w * 32 + (uint32_t)b;
                if (pg >= MAX_PAGES) return 0;
                set_bit(pg);
                if (free_count) free_count--;
                return pg * PAGE_SIZE;
            }
        }
    }
    return 0; /* out of memory */
}

void pmm_free(uint64_t addr) {
    uint32_t pg = addr / PAGE_SIZE;
    if (pg < MAX_PAGES && tst_bit(pg)) {
        clr_bit(pg);
        free_count++;
    }
}

uint64_t pmm_total_bytes(void)  { return total_memory; }
uint32_t pmm_free_pages(void)   { return free_count; }
uint32_t pmm_used_pages(void)   { return total_pages - free_count; }
