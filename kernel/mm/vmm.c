/* kernel/vmm.c — Kernel heap: freelist allocator (v1.1)
 *
 * Replaces the original bump allocator so kfree() actually reclaims memory.
 *
 * Algorithm: explicit free list with first-fit, immediate coalescing.
 *   - Each block carries a header: { size (includes header), free flag }.
 *   - Allocation: scan free list for first block >= requested size.
 *     If found block is much larger, split it.
 *   - Free: mark block free, then merge with adjacent free neighbours.
 *   - 16-byte minimum alignment; header is 16 bytes wide.
 *
 * Reference: "Computer Systems: A Programmer's Perspective" (Bryant & O'Hallaron)
 *            Chapter 9.9 — dynamic memory allocation, explicit free list.
 *
 * Upgrade path (V2): replace first-fit scan with segregated size classes
 *                    (SLAB-style) for O(1) allocation of common sizes.
 */
#include "../types.h"
#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "../log.h"
#include "../klibc.h"
#include "../limits.h"

extern uint8_t kernel_end[];

#define HEAP_SIZE      (32u * 1024u * 1024u)   /* 32 MB heap window */
/* HDR_SIZE computed at compile-time via sizeof; the actual size on x86_64
 * is 24 bytes (4+4+8+8) but we align to 32 to keep all payloads 16-aligned. */
#define HDR_SIZE       32u                      /* sizeof(blk_hdr_t) rounded to 32 */
#define MIN_BLOCK      32u                      /* min useful payload + header */
#define SPLIT_THRESH   64u                      /* don't split if remainder < this */
#define ALIGN16(n)     (((n) + 15u) & ~15u)

/* ---- Block header (16 bytes, always 16-byte aligned) -------------------- */
typedef struct blk_hdr {
    uint32_t        size;     /* total block size including this header */
    uint32_t        free;     /* 1 = free, 0 = allocated                */
    struct blk_hdr *prev;     /* previous block in address order        */
    struct blk_hdr *next_free;/* next FREE block (free list chain)      */
} blk_hdr_t;

static uint8_t  *heap_start;
static uint8_t  *heap_end;
static blk_hdr_t *free_list_head;   /* head of the free block list */

/* ---- Helpers ------------------------------------------------------------ */

static inline blk_hdr_t *next_blk(blk_hdr_t *h) {
    return (blk_hdr_t *)((uint8_t *)h + h->size);
}
static inline void *payload(blk_hdr_t *h) {
    return (uint8_t *)h + HDR_SIZE;
}
static inline blk_hdr_t *hdr_of(void *ptr) {
    return (blk_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
}

/* Remove a block from the free list */
static void fl_remove(blk_hdr_t *h) {
    /* Walk list to find predecessor */
    blk_hdr_t **pp = &free_list_head;
    while (*pp && *pp != h) pp = &(*pp)->next_free;
    if (*pp) *pp = h->next_free;
    h->next_free = NULL;
}

/* Prepend a block to the free list */
static void fl_prepend(blk_hdr_t *h) {
    h->next_free  = free_list_head;
    free_list_head = h;
}

/* Coalesce h with its physical successor if successor is also free */
static blk_hdr_t *coalesce_next(blk_hdr_t *h) {
    blk_hdr_t *nx = next_blk(h);
    if ((uint8_t *)nx >= heap_end) return h;
    if (!nx->free) return h;
    /* Merge: absorb nx into h */
    fl_remove(nx);
    h->size += nx->size;
    /* Fix prev pointer of the block after nx */
    blk_hdr_t *after = next_blk(h);
    if ((uint8_t *)after < heap_end) after->prev = h;
    return h;
}

/* ---- Public API --------------------------------------------------------- */

void vmm_init(void) {
    heap_start = (uint8_t *)(((uintptr_t)kernel_end + PAGE_SIZE - 1)
                              & ~(PAGE_SIZE - 1));
    heap_end   = heap_start + HEAP_SIZE;

    pmm_mark_used((uint64_t)(uintptr_t)heap_start, HEAP_SIZE);

    /* Initialise entire heap as one free block */
    blk_hdr_t *first = (blk_hdr_t *)heap_start;
    first->size       = HEAP_SIZE;
    first->free       = 1;
    first->prev       = NULL;
    first->next_free  = NULL;
    free_list_head    = first;

    kinfo("VMM: freelist heap 0x%llx – 0x%llx (%u KB)\n",
          (unsigned long long)(uintptr_t)heap_start,
          (unsigned long long)(uintptr_t)heap_end,
          HEAP_SIZE / 1024);
}

void *kmalloc(size_t size) {
    if (!size) return NULL;

    uint32_t need = (uint32_t)ALIGN16(size) + HDR_SIZE;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    /* Check memory pressure */
    if (!limits_allow_alloc(size)) {
        kerror("VMM: alloc denied by memory limits (%u bytes)\n",
               (uint32_t)size);
        return NULL;
    }

    /* Disable interrupts to protect the free list from IRQ-context kmalloc */
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));

    /* First-fit search */
    blk_hdr_t *cur = free_list_head;
    while (cur) {
        if (cur->size >= need) break;
        cur = cur->next_free;
    }
    if (!cur) {
        __asm__ volatile ("push %0; popfq" :: "r"(rflags));
        kerror("VMM: heap exhausted (need %u bytes)\n", need);
        return NULL;
    }

    fl_remove(cur);

    /* Split if remainder is large enough to be useful */
    if (cur->size >= need + SPLIT_THRESH) {
        blk_hdr_t *rem = (blk_hdr_t *)((uint8_t *)cur + need);
        rem->size      = cur->size - need;
        rem->free      = 1;
        rem->prev      = cur;
        rem->next_free = NULL;
        /* Fix successor's prev pointer */
        blk_hdr_t *after = next_blk(rem);
        if ((uint8_t *)after < heap_end) after->prev = rem;
        fl_prepend(rem);
        cur->size = need;
    }

    cur->free = 0;
    __asm__ volatile ("push %0; popfq" :: "r"(rflags));
    return payload(cur);
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    blk_hdr_t *h = hdr_of(ptr);

    /* Sanity guard */
    if ((uint8_t *)h < heap_start || (uint8_t *)h >= heap_end || h->free) {
        kerror("VMM: kfree: bad pointer 0x%llx\n",
               (unsigned long long)(uintptr_t)ptr);
        return;
    }

    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));

    h->free = 1;
    fl_prepend(h);

    /* Coalesce with successor, then with predecessor */
    h = coalesce_next(h);
    if (h->prev && h->prev->free) {
        blk_hdr_t *pr = h->prev;
        fl_remove(h);
        pr->size += h->size;
        /* fix successor prev */
        blk_hdr_t *after = next_blk(pr);
        if ((uint8_t *)after < heap_end) after->prev = pr;
        h = pr;
    }
    (void)h;

    __asm__ volatile ("push %0; popfq" :: "r"(rflags));
}

/* krealloc — resize a kmalloc'd block.
 * If ptr is NULL behaves like kmalloc(new_size).
 * If new_size is 0 frees ptr and returns NULL.
 * Tries to grow in-place when the successor block is free; otherwise
 * allocates a new block, copies, and frees the old one. */
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr)       return kmalloc(new_size);
    if (!new_size)  { kfree(ptr); return NULL; }

    blk_hdr_t *h    = hdr_of(ptr);
    uint32_t   need = (uint32_t)ALIGN16(new_size) + HDR_SIZE;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    /* Already large enough — no-op (we keep the block as-is; no shrink) */
    if (h->size >= need) return ptr;

    /* Try in-place growth: coalesce with free successor */
    blk_hdr_t *nx = next_blk(h);
    if ((uint8_t *)nx < heap_end && nx->free && h->size + nx->size >= need) {
        fl_remove(nx);
        h->size += nx->size;
        blk_hdr_t *after = next_blk(h);
        if ((uint8_t *)after < heap_end) after->prev = h;
        /* Split remainder if large enough */
        if (h->size >= need + SPLIT_THRESH) {
            blk_hdr_t *rem = (blk_hdr_t *)((uint8_t *)h + need);
            rem->size      = h->size - need;
            rem->free      = 1;
            rem->prev      = h;
            rem->next_free = NULL;
            blk_hdr_t *af2 = next_blk(rem);
            if ((uint8_t *)af2 < heap_end) af2->prev = rem;
            fl_prepend(rem);
            h->size = need;
        }
        return payload(h);
    }

    /* Fall back: allocate new block, copy, free old */
    void *np = kmalloc(new_size);
    if (!np) return NULL;
    size_t old_data = h->size - HDR_SIZE;
    memcpy(np, ptr, old_data < new_size ? old_data : new_size);
    kfree(ptr);
    return np;
}

size_t vmm_heap_used(void) {
    size_t used = 0;
    uint8_t *cur = heap_start;
    while (cur < heap_end) {
        blk_hdr_t *h = (blk_hdr_t *)cur;
        if (!h->free) used += h->size - HDR_SIZE;
        cur += h->size;
        if (h->size == 0) break; /* safety */
    }
    return used;
}

size_t vmm_heap_reserved(void) {
    size_t free_bytes = 0;
    uint8_t *cur = heap_start;
    while (cur < heap_end) {
        blk_hdr_t *h = (blk_hdr_t *)cur;
        if (h->free) free_bytes += h->size - HDR_SIZE;
        cur += h->size;
        if (h->size == 0) break;
    }
    return free_bytes;
}
