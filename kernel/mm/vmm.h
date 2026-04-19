/* kernel/mm/vmm.h — Kernel virtual memory / heap */
#ifndef VMM_H
#define VMM_H

#include "../types.h"

/* ── Lifecycle ─────────────────────────────────────────────────────────── */
void vmm_init(void);

/* ── Allocation ─────────────────────────────────────────────────────────
 * kmalloc: allocates size bytes + CANARY_SZ guard bytes, returns payload ptr.
 * kzalloc: zero-fills the returned payload (canary bytes stay intact).
 * kfree:   verifies header magic + tail canary, poisons payload with 0xCC,
 *          detects double-free and out-of-range pointers.
 * krealloc: resize; NULL ptr → kmalloc; 0 size → kfree + return NULL.     */
void  *kmalloc  (size_t size);
void  *kzalloc  (size_t size);
void   kfree    (void *ptr);
void  *krealloc (void *ptr, size_t new_size);

/* ── Heap statistics ────────────────────────────────────────────────────── */
size_t   vmm_heap_used     (void);   /* bytes currently allocated (payloads) */
size_t   vmm_heap_reserved (void);   /* bytes in free blocks                 */
uint32_t vmm_alloc_count   (void);   /* live allocated block count           */
uint32_t vmm_total_allocs  (void);   /* cumulative allocations               */
uint32_t vmm_total_frees   (void);   /* cumulative frees                     */
uint32_t vmm_peak_bytes    (void);   /* peak payload bytes ever in use       */

/* ── Heap integrity ─────────────────────────────────────────────────────── *
 * Both names do the same thing — use whichever matches your mental model.
 *
 * Checks every block for:
 *   • valid header magic  (ALLOC_MAGIC / FREE_MAGIC)
 *   • non-zero, in-range block size
 *   • tail canary intact on allocated blocks (catches overflows)
 *
 * Returns 0 if healthy, or count of errors found.
 * Wire to kernel shell command "memcheck". */
int mem_check      (void);
int heap_check_all (void);   /* alias — identical to mem_check() */

#endif /* VMM_H */
