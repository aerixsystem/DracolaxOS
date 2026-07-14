/* tests/test_pmm.c — Physical Memory Manager tests
 *
 * These are kernel-level tests that run during boot when DRACOLAX_TEST=1
 * is defined.  Output goes to VGA + serial.
 *
 * To use: add `#define DRACOLAX_TEST` to main.c before kmain and call
 *         test_pmm() after pmm_init().
 */
#include "../kernel/mm/pmm.h"
#include "../kernel/log.h"
#include "../kernel/klibc.h"

static int pass, fail;

static void check(const char *name, int cond) {
    if (cond) {
        kinfo("  PASS: %s\n", name);
        pass++;
    } else {
        kerror("  FAIL: %s\n", name);
        fail++;
    }
}

void test_pmm(void) {
    kinfo("=== PMM tests ===\n");
    pass = fail = 0;

    /* 1. Total memory reported */
    check("total_bytes > 0", pmm_total_bytes() > 0);

    /* 2. Free page count is sane */
    uint32_t fp = pmm_free_pages();
    check("free_pages > 0", fp > 0);
    check("free_pages < 1M", fp < (1024 * 1024));

    /* 3. Alloc returns aligned page */
    uint32_t p1 = pmm_alloc();
    check("alloc != 0",          p1 != 0);
    check("alloc is page-aligned", (p1 & 0xFFF) == 0);

    /* 4. Free count decreased */
    check("free_pages decreased", pmm_free_pages() == fp - 1);

    /* 5. Alloc another page, distinct address */
    uint32_t p2 = pmm_alloc();
    check("second alloc != 0",     p2 != 0);
    check("two allocs are distinct", p1 != p2);

    /* 6. Free first page, count goes back up */
    pmm_free(p1);
    check("free restores count", pmm_free_pages() == fp - 1);

    /* 7. Re-alloc gets a page (may get p1 back or another) */
    uint32_t p3 = pmm_alloc();
    check("alloc after free works", p3 != 0);

    /* Cleanup */
    pmm_free(p2);
    pmm_free(p3);
    check("final free_pages == original", pmm_free_pages() == fp);

    kinfo("PMM: %d passed, %d failed\n", pass, fail);
}
