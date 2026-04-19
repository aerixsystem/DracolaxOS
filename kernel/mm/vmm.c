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

/* ---- Heap hardening constants ------------------------------------------- *
 * ALLOC_MAGIC  : header signature for a live block.
 * FREE_MAGIC   : header signature for a freed block. kfree() catches double-free
 *                by checking for this value before freeing.
 * TAIL_MAGIC   : 4-byte canary written at the END of every allocated payload.
 *                mem_check() / heap_check_all() verify it on every block walk.
 *                A mismatch means the caller wrote past the end of their buffer.
 * POISON_BYTE  : payload fill value on kfree() — catches use-after-free.
 * -------------------------------------------------------------------------- */
#define ALLOC_MAGIC  0xDEADC0DEu   /* live block header signature   */
#define FREE_MAGIC   0xFEEEFEEEu   /* freed block header signature  */
#define TAIL_MAGIC   0xCAFEBABEu   /* tail canary (overflow guard)  */
#define POISON_BYTE  0xCC          /* use-after-free poison fill    */

/* ---- Allocation tracking counters --------------------------------------- */
static uint32_t g_alloc_count     = 0;  /* live allocated blocks           */
static uint32_t g_total_allocs    = 0;  /* cumulative allocations ever      */
static uint32_t g_total_frees     = 0;  /* cumulative frees ever            */
static uint32_t g_peak_used_bytes = 0;  /* peak payload bytes in use        */
static uint32_t g_current_used_bytes = 0;

/* ---- Block header (32 bytes, always 16-byte aligned) -------------------- */
typedef struct blk_hdr {
    uint32_t        size;      /* total block size including header+canary  */
    uint32_t        magic;     /* ALLOC_MAGIC or FREE_MAGIC                 */
    struct blk_hdr *prev;      /* previous block in address order           */
    struct blk_hdr *next_free; /* next FREE block (free list chain)         */
} blk_hdr_t;

static uint8_t  *heap_start;
static uint8_t  *heap_end;
static blk_hdr_t *free_list_head;   /* head of the free block list */

/* ---- Tail canary helpers ------------------------------------------------ */
/* The tail canary occupies the last 4 bytes of every allocated block's
 * payload region. kmalloc writes it; kfree and mem_check verify it. */
#define CANARY_SZ  4u

static inline uint32_t *tail_canary_ptr(blk_hdr_t *h) {
    /* Payload starts at h + HDR_SIZE.
     * Canary is at payload_end - CANARY_SZ = (h + h->size) - CANARY_SZ. */
    return (uint32_t *)((uint8_t *)h + h->size - CANARY_SZ);
}

static inline void tail_write(blk_hdr_t *h) {
    if (h->size > HDR_SIZE + CANARY_SZ)
        *tail_canary_ptr(h) = TAIL_MAGIC;
}

static inline int tail_ok(blk_hdr_t *h) {
    if (h->size <= HDR_SIZE + CANARY_SZ) return 1; /* too small to canary */
    return (*tail_canary_ptr(h) == TAIL_MAGIC);
}

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
    if (nx->magic != FREE_MAGIC) return h;
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
    first->magic      = FREE_MAGIC;
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

    /* Reserve space for the header AND the 4-byte tail canary inside the block.
     * Previous code allocated HDR_SIZE + ALIGN16(size), meaning the canary
     * was written into memory belonging to the *next* block. */
    uint32_t need = (uint32_t)ALIGN16(size + CANARY_SZ) + HDR_SIZE;
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
        rem->magic     = FREE_MAGIC;
        rem->prev      = cur;
        rem->next_free = NULL;
        /* Fix successor's prev pointer */
        blk_hdr_t *after = next_blk(rem);
        if ((uint8_t *)after < heap_end) after->prev = rem;
        fl_prepend(rem);
        cur->size = need;
    }

    cur->magic = ALLOC_MAGIC;
    tail_write(cur);   /* write TAIL_MAGIC at end of payload — overflow guard */

    /* Update allocation tracking */
    g_alloc_count++;
    g_total_allocs++;
    uint32_t payload_sz = cur->size > HDR_SIZE + CANARY_SZ
                          ? cur->size - HDR_SIZE - CANARY_SZ : 0;
    g_current_used_bytes += payload_sz;
    if (g_current_used_bytes > g_peak_used_bytes)
        g_peak_used_bytes = g_current_used_bytes;

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

    /* Range check */
    if ((uint8_t *)h < heap_start || (uint8_t *)h >= heap_end) {
        kerror("VMM: kfree: ptr 0x%llx out of heap range — ignored\n",
               (unsigned long long)(uintptr_t)ptr);
        return;
    }

    /* Double-free detection: ALLOC_MAGIC means live, FREE_MAGIC means already freed */
    if (h->magic == FREE_MAGIC) {
        kerror("VMM: kfree: DOUBLE FREE detected at 0x%llx — ignored\n",
               (unsigned long long)(uintptr_t)ptr);
        return;   /* do not corrupt the free list */
    }
    if (h->magic != ALLOC_MAGIC) {
        kerror("VMM: kfree: bad magic 0x%08x at 0x%llx — heap corrupt?\n",
               h->magic, (unsigned long long)(uintptr_t)ptr);
        return;
    }

    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));

    /* Tail canary check — detects buffer overflow before we recycle the block */
    if (!tail_ok(h)) {
        kerror("VMM: kfree: OVERFLOW detected at 0x%llx — tail canary corrupt\n",
               (unsigned long long)(uintptr_t)ptr);
        /* Log but continue freeing: better a corrupt free than a leak */
    }

    /* Update allocation tracking */
    uint32_t payload_sz = h->size > (HDR_SIZE + CANARY_SZ)
                          ? h->size - HDR_SIZE - CANARY_SZ : 0;
    if (g_current_used_bytes >= payload_sz)
        g_current_used_bytes -= payload_sz;
    g_alloc_count--;
    g_total_frees++;

    /* Poison payload to catch use-after-free */
    uint32_t full_payload = h->size > HDR_SIZE ? h->size - HDR_SIZE : 0;
    if (full_payload > 0)
        memset(ptr, POISON_BYTE, full_payload);

    h->magic = FREE_MAGIC;
    fl_prepend(h);

    /* Coalesce with successor, then with predecessor */
    h = coalesce_next(h);
    if (h->prev && h->prev->magic == FREE_MAGIC) {
        blk_hdr_t *pr = h->prev;
        fl_remove(h);
        pr->size += h->size;
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
    if ((uint8_t *)nx < heap_end && nx->magic == FREE_MAGIC && h->size + nx->size >= need) {
        fl_remove(nx);
        h->size += nx->size;
        blk_hdr_t *after = next_blk(h);
        if ((uint8_t *)after < heap_end) after->prev = h;
        /* Split remainder if large enough */
        if (h->size >= need + SPLIT_THRESH) {
            blk_hdr_t *rem = (blk_hdr_t *)((uint8_t *)h + need);
            rem->size      = h->size - need;
            rem->magic     = FREE_MAGIC;
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
        if (h->magic == ALLOC_MAGIC) used += h->size - HDR_SIZE;
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
        if (h->magic == FREE_MAGIC) free_bytes += h->size - HDR_SIZE;
        cur += h->size;
        if (h->size == 0) break;
    }
    return free_bytes;
}

uint32_t vmm_alloc_count  (void) { return g_alloc_count;      }
uint32_t vmm_total_allocs (void) { return g_total_allocs;     }
uint32_t vmm_total_frees  (void) { return g_total_frees;      }
uint32_t vmm_peak_bytes   (void) { return g_peak_used_bytes;  }

/* mem_check / heap_check_all — walk the entire heap and report corruption.
 *
 * Checks every block for:
 *   1. Valid header magic (ALLOC_MAGIC or FREE_MAGIC)
 *   2. Sane block size (non-zero, fits within heap)
 *   3. Tail canary intact on allocated blocks (catches buffer overflows)
 *
 * Call from: kernel shell "memcheck", watchdog task, or after suspect ops.
 * Returns: 0 = heap healthy, >0 = number of corrupt blocks detected. */
int mem_check(void) {
    int errors = 0;
    uint32_t total = 0, alloc_blocks = 0, free_blocks = 0;
    uint8_t *cur = heap_start;

    kinfo("MEM_CHECK: walking heap 0x%llx – 0x%llx\n",
          (unsigned long long)(uintptr_t)heap_start,
          (unsigned long long)(uintptr_t)heap_end);

    while (cur < heap_end) {
        blk_hdr_t *h = (blk_hdr_t *)cur;

        /* Sanity: block size must be non-zero and fit within heap */
        if (h->size == 0 || h->size > HEAP_SIZE) {
            kerror("MEM_CHECK: block at 0x%llx: bad size %u — walk aborted\n",
                   (unsigned long long)(uintptr_t)cur, h->size);
            errors++;
            break;
        }

        if (h->magic == ALLOC_MAGIC) {
            alloc_blocks++;
            /* Check tail canary — catches buffer overflow into adjacent block */
            if (!tail_ok(h)) {
                kerror("MEM_CHECK: OVERFLOW at 0x%llx (size=%u) — tail canary corrupt\n",
                       (unsigned long long)(uintptr_t)cur, h->size);
                errors++;
            }
        } else if (h->magic == FREE_MAGIC) {
            free_blocks++;
        } else {
            kerror("MEM_CHECK: CORRUPT header magic 0x%08x at 0x%llx (size=%u)\n",
                   h->magic, (unsigned long long)(uintptr_t)cur, h->size);
            errors++;
        }

        total++;
        cur += h->size;
    }

    if (errors == 0) {
        kinfo("MEM_CHECK: OK — %u blocks (%u alloc, %u free) allocs=%u frees=%u peak=%u KB\n",
              total, alloc_blocks, free_blocks,
              g_total_allocs, g_total_frees,
              g_peak_used_bytes / 1024);
    } else {
        kerror("MEM_CHECK: %d ERROR(S) — allocs=%u frees=%u\n",
               errors, g_total_allocs, g_total_frees);
    }
    return errors;
}

/* heap_check_all — canonical name matching the stability spec.
 * Identical to mem_check(); both names work. */
int heap_check_all(void) { return mem_check(); }
