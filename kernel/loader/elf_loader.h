/* kernel/loader/elf_loader.h */
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "../types.h"

/*
 * Load and execute an ELF32 static binary.
 *
 * Reads from the VFS path, maps PT_LOAD segments, builds user stack
 * with argc/argv/envp/auxv, sets task->abi = ABI_LINUX, and performs
 * a far jump to the ELF entry point.
 *
 * Returns negative errno on failure. Does NOT return on success.
 */
int elf_exec(const char *path, char *const argv[], char *const envp[]);

/* Validate an ELF header in memory. Returns 0 if valid ELF32 i386 exec. */
int elf_validate(const void *ehdr_buf, size_t buf_size);

#endif /* ELF_LOADER_H */
