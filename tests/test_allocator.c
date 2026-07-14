/* tests/test_allocator.c — Unit tests for VMM / allocator
 * Build: gcc -o test_allocator tests/test_allocator.c -Ikernel
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Standalone bump allocator re-implementation for host-side testing */
#define HEAP_SZ (8 * 1024 * 1024)
static unsigned char heap_buf[HEAP_SZ];
static unsigned char *hp = heap_buf;

static void *bump_alloc(size_t sz) {
    sz = (sz + 15) & ~15;
    if (hp + sz > heap_buf + HEAP_SZ) return NULL;
    void *p = hp; hp += sz; return p;
}

static size_t bump_used(void) { return (size_t)(hp - heap_buf); }
static size_t bump_reserved(void) { return (size_t)(heap_buf + HEAP_SZ - hp); }

/* Memory limits logic (mirrors limits.c fix) */
static int limits_ok(size_t total, size_t pmm_used, size_t heap_reserved) {
    size_t real_used = (pmm_used > heap_reserved) ? (pmm_used - heap_reserved) : 0;
    return (real_used * 100 / total) < 90;
}

int run_freelist_tests(void);

int main(void) {
    int pass = 0, fail = 0;

#define CHECK(cond, msg) \
    do { if(cond){ printf("[PASS] %s\n",msg); pass++; } \
         else    { printf("[FAIL] %s\n",msg); fail++; } } while(0)

    /* Test 1: alloc returns non-NULL */
    void *p1 = bump_alloc(64);
    CHECK(p1 != NULL, "alloc 64 bytes returns non-NULL");

    /* Test 2: alignment */
    void *p2 = bump_alloc(3);
    CHECK(((size_t)p2 & 15) == 0, "alloc is 16-byte aligned");

    /* Test 3: used increases */
    size_t u1 = bump_used();
    bump_alloc(1024);
    CHECK(bump_used() > u1, "heap_used increases after alloc");

    /* Test 4: heap exhaustion returns NULL */
    /* Drain remaining space in small chunks */
    { size_t rem = bump_reserved(); if(rem>0) bump_alloc(rem > HEAP_SZ ? HEAP_SZ : rem); }
    hp = heap_buf + HEAP_SZ;  /* force full */
    void *pex = bump_alloc(1);
    CHECK(pex == NULL, "alloc after exhaustion returns NULL");

    /* Test 5: memory limits false-positive fix
     * Simulate: total=128MB, PMM shows 32MB "used" (reserved heap),
     * actual allocations = 1MB.
     * Old code: used = 32MB → 25% — fine actually, but with 90% threshold
     * the REAL issue is larger systems where heap_reserved ~ total RAM. */
    size_t total_kb     = 128 * 1024;  /* 128 MB */
    size_t pmm_used_kb  = 32 * 1024;   /* pre-marked heap window */
    size_t heap_res_kb  = 31 * 1024;   /* 31 MB not yet allocated */
    CHECK(limits_ok(total_kb, pmm_used_kb, heap_res_kb),
          "limits: 1MB actual use on 128MB does not trigger CRITICAL");

    /* Test 6: genuine memory pressure should trigger limit */
    size_t big_used = 116 * 1024; /* 116 of 128 MB = 90.6% actual */
    CHECK(!limits_ok(total_kb, big_used, 0),
          "limits: 90%+ real usage correctly triggers CRITICAL");

    int ff = run_freelist_tests();
    fail += ff;
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

/* ---- Freelist allocator tests ------------------------------------------ */

#define FHEAP_SZ (1024*1024)
static unsigned char fheap[FHEAP_SZ];

typedef struct fhdr {
    unsigned int   sz;
    unsigned int   free;
    struct fhdr   *prev;
    struct fhdr   *next_free;
} fhdr_t;

#define FHDR_SZ 32u  /* matches kernel HDR_SIZE for x86_64 */
#define FALIGN(n) (((n)+15u)&~15u)

static fhdr_t *ffl_head;

static void f_init(void) {
    fhdr_t *h = (fhdr_t*)fheap;
    h->sz = FHEAP_SZ; h->free = 1; h->prev = NULL; h->next_free = NULL;
    ffl_head = h;
}

static void *f_alloc(size_t sz) {
    unsigned need = (unsigned)FALIGN(sz) + FHDR_SZ;
    fhdr_t *cur = ffl_head;
    while (cur && cur->sz < need) cur = cur->next_free;
    if (!cur) return NULL;
    /* remove from list */
    fhdr_t **pp = &ffl_head;
    while (*pp && *pp != cur) pp = &(*pp)->next_free;
    if (*pp) *pp = cur->next_free;
    /* split */
    if (cur->sz >= need + 64) {
        fhdr_t *rem = (fhdr_t*)((unsigned char*)cur + need);
        rem->sz = cur->sz - need; rem->free = 1; rem->prev = cur;
        rem->next_free = ffl_head; ffl_head = rem;
        fhdr_t *af = (fhdr_t*)((unsigned char*)rem + rem->sz);
        if ((unsigned char*)af < fheap + FHEAP_SZ) af->prev = rem;
        cur->sz = need;
    }
    cur->free = 0;
    return (unsigned char*)cur + FHDR_SZ;
}

static void f_free(void *ptr) {
    if (!ptr) return;
    fhdr_t *h = (fhdr_t*)((unsigned char*)ptr - FHDR_SZ);
    h->free = 1;
    h->next_free = ffl_head; ffl_head = h;
    /* coalesce next */
    fhdr_t *nx = (fhdr_t*)((unsigned char*)h + h->sz);
    if ((unsigned char*)nx < fheap + FHEAP_SZ && nx->free) {
        /* remove nx from free list */
        fhdr_t **pp = &ffl_head;
        while (*pp && *pp != nx) pp = &(*pp)->next_free;
        if (*pp) *pp = nx->next_free;
        h->sz += nx->sz;
    }
}

/* append tests invoked from main() */
int run_freelist_tests(void) {
    int p=0,f=0;
#define CK(c,m) do{if(c){printf("[PASS] freelist: %s\n",m);p++;}else{printf("[FAIL] freelist: %s\n",m);f++;}}while(0)

    f_init();
    void *a = f_alloc(100); CK(a!=NULL,"alloc 100 bytes");
    void *b = f_alloc(200); CK(b!=NULL,"alloc 200 bytes");
    CK((char*)b > (char*)a, "second alloc after first");

    /* free b, alloc 200 again - should reuse */
    f_free(b);
    void *c = f_alloc(200); CK(c!=NULL,"alloc after free reuses block");

    /* free a and c, alloc large block - coalesced space available */
    f_free(a); f_free(c);
    void *d = f_alloc(FHEAP_SZ/2); CK(d!=NULL,"large alloc after full coalesce");
    f_free(d);

    /* double-free safety (would corrupt - just skip; tested via valgrind) */

    printf("freelist subtotal: %d passed, %d failed\n",p,f);
    return f;
}

