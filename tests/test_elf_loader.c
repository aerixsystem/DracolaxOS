/* tests/test_elf_loader.c
 * Kernel-level unit test for the ELF loader.
 * Linked into kernel test build; call test_elf_loader() from init.
 */
#include "../kernel/types.h"
#include "../kernel/loader/elf_loader.h"
#include "../kernel/linux/include-uapi/linux/elf.h"
#include "../kernel/klibc.h"
#include "../kernel/drivers/vga/vga.h"
#include "../kernel/log.h"

/* Build a minimal valid ELF32 header in memory and validate it */
void test_elf_loader(void) {
    vga_print("[TEST] elf_validate: ");

    uint8_t buf[sizeof(Elf32_Ehdr)];
    memset(buf, 0, sizeof(buf));
    Elf32_Ehdr *e = (Elf32_Ehdr *)buf;

    /* Set magic */
    e->e_ident[EI_MAG0] = ELFMAG0;
    e->e_ident[EI_MAG1] = ELFMAG1;
    e->e_ident[EI_MAG2] = ELFMAG2;
    e->e_ident[EI_MAG3] = ELFMAG3;
    e->e_ident[EI_CLASS]= ELFCLASS32;
    e->e_ident[EI_DATA] = ELFDATA2LSB;
    e->e_type           = ET_EXEC;
    e->e_machine        = EM_386;
    e->e_phentsize      = sizeof(Elf32_Phdr);
    e->e_phnum          = 0;
    e->e_phoff          = sizeof(Elf32_Ehdr);

    int r = elf_validate(buf, sizeof(buf));
    if (r == 0) {
        vga_print("PASS (valid ELF32 accepted)\n");
    } else {
        vga_print("FAIL (valid ELF32 rejected)\n");
    }

    /* Test invalid magic */
    vga_print("[TEST] elf_validate bad magic: ");
    buf[0] = 0x00;
    r = elf_validate(buf, sizeof(buf));
    if (r < 0) {
        vga_print("PASS (bad magic rejected)\n");
    } else {
        vga_print("FAIL (bad magic accepted)\n");
    }

    /* Test too-small buffer */
    vga_print("[TEST] elf_validate short buf: ");
    r = elf_validate(buf, 4);
    if (r < 0) {
        vga_print("PASS (short buf rejected)\n");
    } else {
        vga_print("FAIL (short buf accepted)\n");
    }

    kinfo("[TEST] elf_loader tests complete\n");
}
