/* kernel/vmm.h — Kernel virtual memory / heap */
#ifndef VMM_H
#include "../types.h"
#define VMM_H


/* Initialise the kernel heap (must be called after paging + PMM) */
void vmm_init(void);

/* Allocate size bytes, aligned to 16 bytes; returns NULL on failure */
void *kmalloc(size_t size);

/* Allocate and zero-fill */
void *kzalloc(size_t size);

/* Free a previously kmalloc'd pointer */
void  kfree(void *ptr);

/* Resize a kmalloc'd block (NULL ptr → kmalloc; 0 size → kfree) */
void *krealloc(void *ptr, size_t new_size);

/* Report current heap usage */
size_t vmm_heap_used(void);

#endif /* VMM_H */

/* New in v1.1: bytes in the heap window not yet handed to callers.
 * Used by limits.c to correct memory pressure accounting. */
size_t vmm_heap_reserved(void);
