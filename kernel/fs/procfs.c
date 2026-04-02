/* kernel/procfs.c — Minimal /proc implementation
 *
 * Provides:
 *   /proc/cmdline   — kernel command line
 *   /proc/meminfo   — memory statistics
 *   /proc/self      — symlink to /proc/<current_pid>
 *   /proc/self/exe  — current task name
 *   /proc/self/maps — stub
 */
#include "../types.h"
#include "vfs.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"
#include "../klibc.h"
#include "../log.h"
#include "procfs.h"

/* ---- synthetic file node ------------------------------------------------ */

typedef void (*proc_fill_fn)(char *buf, size_t sz);

typedef struct {
    char          name[VFS_NAME_MAX];
    proc_fill_fn  fill;
    char          data[512]; /* cached rendered content */
    uint32_t      size;
} proc_file_t;

#define PROC_FILES_MAX 16
static proc_file_t proc_files[PROC_FILES_MAX];
static int         proc_nfiles = 0;

/* Underlying vfs_node_t array */
static vfs_node_t proc_nodes[PROC_FILES_MAX];
static vfs_node_t proc_root_node;

/* ---- fill functions ------------------------------------------------------ */

static void fill_cmdline(char *buf, size_t sz) {
    strncpy(buf, "dracolaxos quiet\n", sz);
}

static void fill_meminfo(char *buf, size_t sz) {
    uint32_t total_kb = (uint32_t)(pmm_total_bytes() / 1024);
    uint32_t free_kb  = pmm_free_pages() * 4;
    uint32_t used_kb  = pmm_used_pages() * 4;
    snprintf(buf, sz,
        "MemTotal:       %7u kB\n"
        "MemFree:        %7u kB\n"
        "MemUsed:        %7u kB\n"
        "Buffers:              0 kB\n"
        "Cached:               0 kB\n"
        "HeapUsed:       %7u kB\n",
        total_kb, free_kb, used_kb,
        vmm_heap_used() / 1024);
}

static void fill_self_exe(char *buf, size_t sz) {
    strncpy(buf, sched_current()->name, sz);
}

static void fill_self_maps(char *buf, size_t sz) {
    /* FIX: populate /proc/self/maps with real kernel regions.
     * linker.ld provides:  kernel_bss_end / kernel_end (both mark image end).
     * Kernel is loaded at 0x100000.  Text, rodata, data/bss are consecutive.
     * Heap starts above kernel_end (page-aligned). */
    extern char kernel_end[];        /* PROVIDE(kernel_end = .) in linker.ld */
    extern char kernel_bss_start[];  /* .bss section start                   */

    uint64_t k_base     = 0x100000ULL;
    uint64_t k_bss_st   = (uint64_t)(uintptr_t)kernel_bss_start;
    uint64_t k_end      = (uint64_t)(uintptr_t)kernel_end;
    uint64_t heap_base  = (k_end + 0xFFFu) & ~(uint64_t)0xFFF;
    uint64_t heap_end   = heap_base + (uint64_t)vmm_heap_used();
    uint32_t total_kb   = (uint32_t)(pmm_total_bytes() / 1024);

    snprintf(buf, sz,
        "%08llx-%08llx r-xp 00000000 00:00 0  [kernel.text+rodata]\n"
        "%08llx-%08llx rw-p 00000000 00:00 0  [kernel.data+bss]\n"
        "%08llx-%08llx rw-p 00000000 00:00 0  [heap]\n"
        "# RAM total: %u kB\n",
        (unsigned long long)k_base,
        (unsigned long long)k_bss_st,
        (unsigned long long)k_bss_st,
        (unsigned long long)k_end,
        (unsigned long long)heap_base,
        (unsigned long long)heap_end,
        total_kb);
}

/* ---- VFS ops ------------------------------------------------------------ */

static int proc_read(vfs_node_t *node, uint32_t off, uint32_t len,
                     uint8_t *buf) {
    proc_file_t *pf = (proc_file_t *)node->priv;
    if (!pf) return -1;

    /* Re-render on every read so values are fresh */
    pf->fill(pf->data, sizeof(pf->data));
    pf->size = (uint32_t)strlen(pf->data);
    node->size = pf->size;

    if (off >= pf->size) return 0;
    uint32_t avail = pf->size - off;
    uint32_t n     = (len < avail) ? len : avail;
    memcpy(buf, pf->data + off, n);
    return (int)n;
}

static int proc_write(vfs_node_t *n, uint32_t o, uint32_t l,
                      const uint8_t *b) { (void)n;(void)o;(void)l;(void)b;return -1; }

static int proc_readdir(vfs_node_t *dir, uint32_t idx,
                        char *name_out, size_t max) {
    (void)dir;
    if ((int)idx >= proc_nfiles) return -1;
    strncpy(name_out, proc_files[idx].name, max);
    return 0;
}

static vfs_node_t *proc_finddir(vfs_node_t *dir, const char *name) {
    (void)dir;
    for (int i = 0; i < proc_nfiles; i++) {
        if (strcmp(proc_files[i].name, name) == 0)
            return &proc_nodes[i];
    }
    return NULL;
}

static vfs_ops_t proc_file_ops = {
    .read    = proc_read,
    .write   = proc_write,
    .readdir = NULL,
    .finddir = NULL,
};

static vfs_ops_t proc_dir_ops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = proc_readdir,
    .finddir = proc_finddir,
};

/* ---- registration helper ------------------------------------------------ */

static void proc_reg(const char *name, proc_fill_fn fn) {
    if (proc_nfiles >= PROC_FILES_MAX) return;
    proc_file_t *pf = &proc_files[proc_nfiles];
    vfs_node_t  *n  = &proc_nodes[proc_nfiles];

    strncpy(pf->name, name, VFS_NAME_MAX - 1);
    pf->fill = fn;
    pf->fill(pf->data, sizeof(pf->data));
    pf->size = (uint32_t)strlen(pf->data);

    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->type  = VFS_TYPE_FILE;
    n->size  = pf->size;
    n->ops   = &proc_file_ops;
    n->priv  = pf;

    proc_nfiles++;
}

/* ---- public API --------------------------------------------------------- */

void procfs_init(void) {
    /* Set up root dir node */
    strncpy(proc_root_node.name, "proc", VFS_NAME_MAX - 1);
    proc_root_node.type = VFS_TYPE_DIR;
    proc_root_node.size = 0;
    proc_root_node.ops  = &proc_dir_ops;
    proc_root_node.priv = NULL;

    /* Register synthetic files */
    proc_reg("cmdline",    fill_cmdline);
    proc_reg("meminfo",    fill_meminfo);
    proc_reg("self/exe",   fill_self_exe);
    proc_reg("self/maps",  fill_self_maps);

    vfs_mount("/proc", &proc_root_node);
    kinfo("PROC: mounted at /proc (%d files)\n", proc_nfiles);
}

vfs_node_t *procfs_root(void) { return &proc_root_node; }
