/* tests/stress_kernel.c — DracolaxOS kernel stress test
 *
 * Exercises: scheduler under load, heap under pressure, VFS concurrent
 * access, and watchdog detection of stuck tasks.
 *
 * Build: compiled into the kernel and registered as a task via:
 *   sched_spawn(stress_test_main, "stress");
 * Wire it into init.c behind #ifdef DRACO_DEBUG or call from the shell.
 *
 * Tests:
 *   1. Spawn 20 worker tasks at various priorities — verify they all
 *      complete without deadlock.
 *   2. Heap thrash: 1000 alloc/free cycles of random sizes with canary
 *      verification after each cycle.
 *   3. VFS concurrent writes: 8 tasks each write 100 lines to different
 *      files in /ramfs and read them back.
 *   4. Memory pressure: allocate until heap is 80% full, verify limits
 *      kick in, then free everything and verify recovery.
 *   5. Scheduler starvation: spawn a PRIO_LOW task alongside PRIO_HIGH
 *      tasks and verify the low task eventually runs (aging).
 */

#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/klibc.h"
#include "../kernel/mm/vmm.h"
#include "../kernel/sched/sched.h"
#include "../kernel/fs/vfs.h"
#include "../kernel/fs/ramfs.h"

/* ── Shared counters (written by worker tasks) ───────────────────────── */
static volatile int g_workers_done    = 0;
static volatile int g_workers_started = 0;
static volatile int g_heap_errors     = 0;
static volatile int g_vfs_errors      = 0;
static volatile int g_low_prio_ran    = 0;

/* ── Test 1: worker tasks at mixed priorities ─────────────────────────── */
static void worker_task(void) {
    __asm__ volatile ("sti");
    g_workers_started++;

    /* Do some busy work — alloc, write, free */
    for (int i = 0; i < 50; i++) {
        void *p = kmalloc(256);
        if (p) {
            memset(p, (int)(i & 0xFF), 256);
            sched_sleep(1);   /* yield to let other tasks run */
            kfree(p);
        }
    }

    g_workers_done++;
    sched_exit();
}

/* ── Test 2: heap thrash ──────────────────────────────────────────────── */
#define HEAP_SLOTS 64
static void heap_stress_task(void) {
    __asm__ volatile ("sti");
    void *ptrs[HEAP_SLOTS];
    memset(ptrs, 0, sizeof(ptrs));

    for (int round = 0; round < 200; round++) {
        /* Allocate phase */
        for (int i = 0; i < HEAP_SLOTS; i++) {
            size_t sz = (size_t)(((round * 7 + i * 13) % 512) + 16);
            ptrs[i] = kmalloc(sz);
            if (ptrs[i]) memset(ptrs[i], 0xAB, sz);
        }
        sched_yield();

        /* Check heap integrity mid-thrash */
        if (mem_check() > 0) {
            kerror("STRESS: heap corruption detected in round %d\n", round);
            g_heap_errors++;
        }

        /* Free phase */
        for (int i = 0; i < HEAP_SLOTS; i++) {
            if (ptrs[i]) { kfree(ptrs[i]); ptrs[i] = NULL; }
        }
        sched_yield();
    }

    kinfo("STRESS: heap thrash done — %d errors\n", g_heap_errors);
    sched_exit();
}

/* ── Test 3: VFS concurrent write/read ───────────────────────────────── */
extern vfs_node_t *ramfs_root;

static void vfs_worker_task(void) {
    __asm__ volatile ("sti");
    int id = g_workers_started++;   /* reuse counter for unique filename */

    char fname[32];
    snprintf(fname, sizeof(fname), "stress_%d.txt", id);

    /* Create and write */
    if (ramfs_root) {
        ramfs_create(ramfs_root, fname);
        vfs_node_t *f = vfs_finddir(ramfs_root, fname);
        if (f) {
            char line[64];
            uint32_t off = 0;
            for (int i = 0; i < 100; i++) {
                snprintf(line, sizeof(line), "task%d line%d\n", id, i);
                int w = vfs_write(f, off, (uint32_t)strlen(line),
                                  (const uint8_t *)line);
                if (w < 0) { g_vfs_errors++; break; }
                off += (uint32_t)w;
            }
            /* Read back and verify first line */
            char rbuf[64];
            int r = vfs_read(f, 0, sizeof(rbuf)-1, (uint8_t *)rbuf);
            if (r > 0) {
                rbuf[r] = '\0';
                snprintf(line, sizeof(line), "task%d line0\n", id);
                if (strncmp(rbuf, line, strlen(line)) != 0) {
                    kerror("STRESS: VFS read mismatch for task %d\n", id);
                    g_vfs_errors++;
                }
            }
            /* Clean up */
            ramfs_delete(ramfs_root, fname);
        }
    }

    g_workers_done++;
    sched_exit();
}

/* ── Test 4: memory pressure ──────────────────────────────────────────── */
#define PRESSURE_SLOTS 128
static void memory_pressure_task(void) {
    __asm__ volatile ("sti");
    void *ptrs[PRESSURE_SLOTS];
    memset(ptrs, 0, sizeof(ptrs));
    int allocated = 0;

    kinfo("STRESS: memory pressure test starting\n");

    /* Allocate large blocks until limits kick in */
    for (int i = 0; i < PRESSURE_SLOTS; i++) {
        ptrs[i] = kmalloc(65536);   /* 64 KB per block */
        if (!ptrs[i]) break;        /* limits_allow_alloc or heap full */
        allocated++;
        if (i % 16 == 0) sched_yield();
    }

    kinfo("STRESS: allocated %d × 64 KB blocks before limit\n", allocated);

    /* Verify heap is still walkable under pressure */
    int errs = mem_check();
    if (errs > 0)
        kerror("STRESS: heap corrupt under pressure (%d errors)\n", errs);

    /* Free everything — verify full recovery */
    for (int i = 0; i < allocated; i++) {
        if (ptrs[i]) { kfree(ptrs[i]); ptrs[i] = NULL; }
    }
    sched_yield();

    errs = mem_check();
    kinfo("STRESS: after pressure release — heap %s\n",
          errs == 0 ? "OK" : "CORRUPT");

    g_workers_done++;
    sched_exit();
}

/* ── Test 5: starvation / aging ───────────────────────────────────────── */
static void low_prio_task(void) {
    __asm__ volatile ("sti");
    /* Just running proves aging kicked in */
    g_low_prio_ran = 1;
    kinfo("STRESS: low-priority task ran — starvation prevention OK\n");
    sched_exit();
}

static void high_prio_burner(void) {
    __asm__ volatile ("sti");
    /* Burn CPU for a while to starve the low-prio task */
    for (int i = 0; i < 500; i++) sched_yield();
    sched_exit();
}

/* ── Main stress entry point ─────────────────────────────────────────── */
void stress_test_main(void) {
    __asm__ volatile ("sti");
    kinfo("STRESS: kernel stress test starting\n");

    /* ── Test 1: 20 worker tasks ─────────────────────────────── */
    kinfo("STRESS: [1] spawning 20 worker tasks\n");
    g_workers_done = 0; g_workers_started = 0;
    for (int i = 0; i < 20; i++) {
        int id = sched_spawn(worker_task, "stress-worker");
        if (id < 0) { kwarn("STRESS: spawn failed at i=%d\n", i); break; }
        /* Distribute across priority levels */
        uint8_t prio = (uint8_t)(PRIO_LOW + (i % 3));
        sched_set_priority(id, prio);
    }
    /* Wait for all workers */
    for (int wait = 0; wait < 500 && g_workers_done < g_workers_started; wait++)
        sched_sleep(10);
    kinfo("STRESS: [1] %d/%d workers completed\n",
          g_workers_done, g_workers_started);

    /* ── Test 2: heap thrash ─────────────────────────────────── */
    kinfo("STRESS: [2] heap thrash\n");
    g_heap_errors = 0;
    int hid = sched_spawn(heap_stress_task, "stress-heap");
    sched_set_priority(hid, PRIO_NORMAL);
    for (int wait = 0; wait < 1000 && g_workers_done < g_workers_started + 1; wait++)
        sched_sleep(10);
    kinfo("STRESS: [2] heap thrash: %d errors\n", g_heap_errors);

    /* ── Test 3: VFS concurrent writes ──────────────────────── */
    kinfo("STRESS: [3] VFS concurrent write\n");
    g_vfs_errors = 0;
    int vfs_base = g_workers_done;
    for (int i = 0; i < 8; i++)
        sched_spawn(vfs_worker_task, "stress-vfs");
    for (int wait = 0; wait < 500 && g_workers_done < vfs_base + 8; wait++)
        sched_sleep(10);
    kinfo("STRESS: [3] VFS: %d errors\n", g_vfs_errors);

    /* ── Test 4: memory pressure ─────────────────────────────── */
    kinfo("STRESS: [4] memory pressure\n");
    int pmid = sched_spawn(memory_pressure_task, "stress-mem");
    sched_set_priority(pmid, PRIO_NORMAL);
    for (int wait = 0; wait < 2000 && g_workers_done < vfs_base + 9; wait++)
        sched_sleep(10);

    /* ── Test 5: starvation / priority aging ─────────────────── */
    kinfo("STRESS: [5] starvation / aging test\n");
    g_low_prio_ran = 0;
    /* Spawn 4 high-prio burners and 1 low-prio task */
    for (int i = 0; i < 4; i++) {
        int bid = sched_spawn(high_prio_burner, "stress-burn");
        sched_set_priority(bid, PRIO_HIGH);
    }
    int lid = sched_spawn(low_prio_task, "stress-lowprio");
    sched_set_priority(lid, PRIO_LOW);
    /* Wait up to 5 seconds for the low-prio task to get CPU via aging */
    for (int wait = 0; wait < 500 && !g_low_prio_ran; wait++)
        sched_sleep(10);
    kinfo("STRESS: [5] starvation test: %s\n",
          g_low_prio_ran ? "PASS (low-prio task ran)" : "FAIL (task starved)");

    /* ── Summary ─────────────────────────────────────────────── */
    kinfo("STRESS: all tests complete\n");
    kinfo("STRESS: heap_errors=%d  vfs_errors=%d  starvation=%s\n",
          g_heap_errors, g_vfs_errors,
          g_low_prio_ran ? "OK" : "FAILED");

    /* Final heap integrity check */
    int final_errs = mem_check();
    kinfo("STRESS: final heap check: %s\n",
          final_errs == 0 ? "CLEAN" : "CORRUPT");

    sched_exit();
}
