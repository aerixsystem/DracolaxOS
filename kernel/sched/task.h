/* kernel/task.h — Task structure (x86_64) */
#ifndef TASK_H
#define TASK_H
#include "../types.h"
#include "../fs/vfs.h"
#include "../ipc/signal.h"

#define TASK_MAX        16
#define TASK_STACK_SZ   16384u  /* 16 KB per task (64-bit needs more) */
#define TASK_NAME_LEN   32
#define TASK_FD_MAX     32

#define ABI_DRACO  0
#define ABI_LINUX  1

typedef enum {
    TASK_EMPTY = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD,
} task_state_t;

typedef struct task {
    /* ---- scheduler state ---------------------------------------------- */
    uint64_t     rsp;           /* saved stack pointer (x86_64)             */
    uint32_t     id;
    task_state_t state;
    char         name[TASK_NAME_LEN];
    uint8_t     *stack;
    uint64_t     sleep_until;   /* tick count to wake at                    */

    /* ---- identity ----------------------------------------------------- */
    uint32_t     pid;
    uint32_t     ppid;
    uint32_t     abi;           /* ABI_DRACO or ABI_LINUX                   */

    /* ---- user-space address space ------------------------------------- */
    uint64_t     user_entry;
    uint64_t     user_stack;
    uint64_t     brk_start;
    uint64_t     brk_current;

    /* ---- file descriptor table ---------------------------------------- */
    vfs_node_t  *fd_table[TASK_FD_MAX];

    /* ---- exit status -------------------------------------------------- */
    int32_t      exit_code;

    /* ---- mmap region table -------------------------------------------- */
    /* Points to a heap-allocated lx_mmap_region_t[LX_MMAP_MAX] array.
     * Allocated lazily on first mmap; freed by lx_munmap_all() on exit. */
    void        *mmap_regions;

    /* ---- signal state (FIX 3.8) --------------------------------------- */
    signal_state_t signals;
} task_t;

#endif /* TASK_H */
