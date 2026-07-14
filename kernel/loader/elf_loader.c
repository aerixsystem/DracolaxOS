/* kernel/loader/elf_loader.c
 *
 * ELF64 static binary loader for DracolaxOS x86_64.
 *
 * Supports:
 *   - ET_EXEC ELF64 (EM_X86_64)
 *   - PT_LOAD segments (R, RW, RX)
 *   - Static binaries (no PT_INTERP / dynamic linker)
 *   - Initial stack: argc, argv[], envp[], auxv[]
 *     (System V AMD64 ABI initial process stack layout)
 *
 * Limitations:
 *   - No ASLR
 *   - No dynamic linker / shared libraries
 *   - Runs in ring-0 kernel space (ring-3 requires TSS — future work)
 */
#include "../types.h"
#include "../fs/vfs.h"
#include "../fs/ramfs.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../klibc.h"
#include "../log.h"
#include "../sched/sched.h"
#include "../sched/task.h"
#include "../linux/include-uapi/linux/elf.h"
#include "../linux/linux_syscall.h"
#include "../linux/include-uapi/asm-generic/errno.h"

/* ---- constants ---------------------------------------------------------- */
#define USER_STACK_TOP  0x800000ULL     /* 8 MB: top of user stack          */
#define USER_STACK_SIZE 0x020000ULL     /* 128 KB stack                     */
#define ELF_MAX_SIZE    (4u*1024u*1024u) /* max binary: 4 MB                */

/* ---- helpers ------------------------------------------------------------ */

static inline int is_elf64(const Elf64_Ehdr *e) {
    return e->e_ident[EI_MAG0] == ELFMAG0 &&
           e->e_ident[EI_MAG1] == ELFMAG1 &&
           e->e_ident[EI_MAG2] == ELFMAG2 &&
           e->e_ident[EI_MAG3] == ELFMAG3 &&
           e->e_ident[EI_CLASS] == ELFCLASS64 &&
           e->e_ident[EI_DATA]  == ELFDATA2LSB &&
           e->e_type            == ET_EXEC &&
           e->e_machine         == EM_X86_64;
}

int elf_validate(const void *buf, size_t sz) {
    if (sz < sizeof(Elf64_Ehdr)) return -ENOEXEC;
    const Elf64_Ehdr *e = (const Elf64_Ehdr *)buf;
    if (!is_elf64(e)) return -ENOEXEC;
    if (e->e_phentsize < sizeof(Elf64_Phdr)) return -ENOEXEC;
    if (e->e_phoff + (uint64_t)e->e_phnum * e->e_phentsize > sz) return -ENOEXEC;
    return 0;
}

/* Map a PT_LOAD segment. Identity-map: virt==phys for first 4 GB. */
static int load_segment(const Elf64_Phdr *ph, const uint8_t *file_data) {
    if (ph->p_memsz == 0) return 0;

    uint64_t vaddr  = ph->p_vaddr;
    uint64_t memsz  = ph->p_memsz;
    uint64_t filesz = ph->p_filesz;
    uint64_t page_start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t page_end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    uint32_t pflags = PAGE_PRESENT;
    if (ph->p_flags & PF_W) pflags |= PAGE_WRITE;

    for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc();
        if (!pa) {
            kwarn("ELF: OOM at vaddr 0x%llx\n", (unsigned long long)va);
            return -ENOMEM;
        }
        paging_map(va, pa, pflags);
        memset((void *)(uintptr_t)va, 0, PAGE_SIZE);
    }

    if (filesz > 0 && file_data)
        memcpy((void *)(uintptr_t)vaddr, file_data + ph->p_offset, (size_t)filesz);

    kdebug("ELF: PT_LOAD vaddr=0x%llx filesz=%llu memsz=%llu flags=0x%x\n",
           (unsigned long long)vaddr, (unsigned long long)filesz,
           (unsigned long long)memsz, (unsigned)ph->p_flags);
    return 0;
}

/* Build the initial user stack (System V AMD64 ABI).
 *
 * Stack layout at RSP (low → high):
 *   argc          (8 bytes)
 *   argv[0..n-1]  (8 bytes each, pointers)
 *   NULL          (argv terminator)
 *   envp[0..m-1]  (8 bytes each, pointers)
 *   NULL          (envp terminator)
 *   auxv pairs    (AT_type, value, 8 bytes each)
 *   AT_NULL pair
 *   [string data]
 *
 * Returns new RSP, or 0 on OOM.
 */
static uint64_t build_user_stack(const Elf64_Ehdr *ehdr,
                                  char *const argv[], char *const envp[]) {
    uint8_t *stack_base = (uint8_t *)kmalloc(USER_STACK_SIZE);
    if (!stack_base) return 0;
    memset(stack_base, 0, USER_STACK_SIZE);

    uint64_t stk_virt = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t i = 0; i < USER_STACK_SIZE / PAGE_SIZE; i++) {
        uint64_t pa = (uint64_t)(uintptr_t)(stack_base + i * PAGE_SIZE);
        paging_map(stk_virt + i * PAGE_SIZE, pa, PAGE_PRESENT | PAGE_WRITE);
    }

    int argc = 0, envc = 0;
    if (argv) while (argv[argc]) argc++;
    if (envp) while (envp[envc]) envc++;

    uint64_t sp = USER_STACK_TOP;

    /* Temporary storage for string virtual addresses */
    uint64_t *argp  = (uint64_t *)kmalloc(sizeof(uint64_t) * (size_t)(argc + 1));
    uint64_t *envp2 = (uint64_t *)kmalloc(sizeof(uint64_t) * (size_t)(envc + 1));
    if (!argp || !envp2) return 0;

    /* Push string bytes at top of stack (high to low) */
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        sp -= (uint64_t)l;
        memcpy((void *)(uintptr_t)sp, argv[i], l);
        argp[i] = sp;
    }
    argp[argc] = 0;
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp[i]) + 1;
        sp -= (uint64_t)l;
        memcpy((void *)(uintptr_t)sp, envp[i], l);
        envp2[i] = sp;
    }
    envp2[envc] = 0;

    sp &= ~15ULL;   /* 16-byte align before pushing structured data */

#define PUSH64(v) do { sp -= 8ULL; *(uint64_t *)(uintptr_t)sp = (uint64_t)(v); } while(0)

    /* auxv — push in reverse so AT_NULL ends up at bottom */
    PUSH64(0);                              PUSH64(AT_NULL);
    PUSH64(0);                              PUSH64(AT_SECURE);
    PUSH64(0);                              PUSH64(AT_EGID);
    PUSH64(0);                              PUSH64(AT_GID);
    PUSH64(0);                              PUSH64(AT_EUID);
    PUSH64(0);                              PUSH64(AT_UID);
    PUSH64(ehdr->e_entry);                  PUSH64(AT_ENTRY);
    PUSH64(ehdr->e_phnum);                  PUSH64(AT_PHNUM);
    PUSH64(ehdr->e_phentsize);              PUSH64(AT_PHENT);
    PUSH64(ehdr->e_phoff + ehdr->e_entry);  PUSH64(AT_PHDR);
    PUSH64(PAGE_SIZE);                      PUSH64(AT_PAGESZ);

    /* envp pointer array */
    PUSH64(0);
    for (int i = envc - 1; i >= 0; i--) PUSH64(envp2[i]);

    /* argv pointer array */
    PUSH64(0);
    for (int i = argc - 1; i >= 0; i--) PUSH64(argp[i]);

    /* argc */
    PUSH64((uint64_t)argc);

#undef PUSH64

    return sp;
}

/* ---- elf_exec ------------------------------------------------------------ */

int elf_exec(const char *path, char *const argv[], char *const envp[]) {
    if (!path) return -EFAULT;

    char full[128];
    const char *lookup = path;
    if (path[0] != '/') {
        snprintf(full, sizeof(full), "/storage/main/apps/%s", path);
        lookup = full;
    }

    vfs_node_t *node = vfs_open(lookup);
    if (!node) { kwarn("ELF: %s not found\n", lookup); return -ENOENT; }
    if (node->size == 0 || node->size > ELF_MAX_SIZE) {
        kwarn("ELF: bad size %u for %s\n", node->size, lookup);
        return -ENOEXEC;
    }

    uint8_t *data = (uint8_t *)kmalloc(node->size);
    if (!data) return -ENOMEM;

    int n = vfs_read(node, 0, node->size, data);
    if (n <= 0 || (uint32_t)n < node->size) {
        kwarn("ELF: short read %d / %u\n", n, node->size);
        return -EIO;
    }

    int r = elf_validate(data, (size_t)n);
    if (r < 0) { kwarn("ELF: invalid ELF64 header in %s\n", path); return r; }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;

    /* Reject dynamic binaries */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(data + ehdr->e_phoff +
                                         (uint64_t)i * ehdr->e_phentsize);
        if (ph->p_type == PT_INTERP) {
            kwarn("ELF: dynamic binary not supported: %s\n", path);
            return -ENOEXEC;
        }
    }

    /* Load PT_LOAD segments */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(data + ehdr->e_phoff +
                                         (uint64_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        r = load_segment(ph, data);
        if (r < 0) return r;
    }

    /* Build initial stack */
    char *default_argv[] = { (char *)path, NULL };
    char *default_envp[] = { "TERM=xterm-256color", "HOME=/", "PATH=/ramfs", NULL };
    if (!argv || !argv[0]) argv = default_argv;
    if (!envp)              envp = default_envp;

    uint64_t user_rsp = build_user_stack(ehdr, argv, envp);
    if (!user_rsp) return -ENOMEM;

    task_t *t     = sched_current();
    t->abi        = ABI_LINUX;
    t->user_entry = ehdr->e_entry;
    t->user_stack = user_rsp;
    strncpy(t->name, path, TASK_NAME_LEN - 1);

    kinfo("ELF: entry=0x%llx rsp=0x%llx %s\n",
          (unsigned long long)ehdr->e_entry,
          (unsigned long long)user_rsp, path);

    /* Jump to ELF _start.
     * RSP = initial process stack (argc at top per AMD64 ABI).
     * Both %0 and %1 are forced to 64-bit registers via uint64_t — no
     * operand size mismatch with jmp. */
    uint64_t entry64 = ehdr->e_entry;
    __asm__ volatile (
        "mov  %0, %%rsp\n"       /* load initial RSP */
        "xor  %%rax, %%rax\n"   /* clear rax        */
        "jmp  *%1\n"             /* 64-bit indirect jump to _start */
        :: "r"(user_rsp), "r"(entry64)
        : "rax", "memory"
    );

    __builtin_unreachable();
}
