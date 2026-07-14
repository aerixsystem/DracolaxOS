/* kernel/linker.ld — DracolaxOS x86_64 kernel linker script (improved)
 *
 * Key improvements over v1:
 *  - Explicit ALIGN(16) on .text for correct function alignment
 *  - .rodata is read-only (no WRITE flag) — catches accidental writes at link time
 *  - Separate .data.rel.ro for relocated read-only data (RELRO)
 *  - Explicit page alignment (4096) on each major section so the PMM/VMM
 *    can enforce R/W/X permissions per-section in future
 *  - kernel_text_start / kernel_text_end symbols for future NX mapping
 *  - kernel_rodata_start / kernel_rodata_end for future write-protect
 *  - .init_array / .fini_array discarded (no C++ static ctors/dtors in kernel)
 *  - .eh_frame_hdr discarded (no stack unwinding in kernel)
 *  - ASSERT that kernel stays below 16 MB (guards against huge data sections)
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)        /* boot.s: 32→64-bit long mode bridge, then kmain */

SECTIONS
{
    /* Load at 1 MB — above BIOS data area and legacy real-mode regions */
    . = 0x100000;

    /* ------------------------------------------------------------------ */
    /* Multiboot2 header: must appear within the first 32 KB of the image */
    /* ------------------------------------------------------------------ */
    .multiboot : {
        KEEP(*(.multiboot))
    }

    /* ------------------------------------------------------------------ */
    /* Executable code                                                     */
    /* ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(kernel_text_start = .);

    .text : ALIGN(16) {
        *(.text.unlikely .text.startup)   /* cold paths first — cache friendly */
        *(.text*)
    }

    PROVIDE(kernel_text_end = .);

    /* ------------------------------------------------------------------ */
    /* Read-only data (constants, string literals, font tables, etc.)     */
    /* ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(kernel_rodata_start = .);

    .rodata : {
        *(.rodata*)
    }

    /* Relocated read-only data (const pointers, vtable-like structures)  */
    .data.rel.ro : ALIGN(4096) {
        *(.data.rel.ro*)
    }

    PROVIDE(kernel_rodata_end = .);

    /* ------------------------------------------------------------------ */
    /* Initialised data                                                    */
    /* ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(kernel_data_start = .);

    .data : {
        *(.data*)
    }

    PROVIDE(kernel_data_end = .);

    /* ------------------------------------------------------------------ */
    /* Uninitialised / zero-initialised data                              */
    /* ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(kernel_bss_start = .);

    .bss : {
        *(COMMON)       /* legacy C tentative definitions */
        *(.bss*)
    }

    . = ALIGN(4096);
    PROVIDE(kernel_bss_end = .);

    /* ------------------------------------------------------------------ */
    /* End-of-kernel marker — VMM heap starts here (page-aligned)        */
    /* ------------------------------------------------------------------ */
    PROVIDE(kernel_end = .);

    /* Safety check: kernel image must fit in 16 MB so the identity map
     * built in boot.s (which covers 0–16 MB) is always sufficient.      */
    ASSERT(. <= 0x1000000, "kernel image exceeds 16 MB — increase boot.s page tables")

    /* ------------------------------------------------------------------ */
    /* Discarded sections                                                  */
    /* ------------------------------------------------------------------ */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.GNU-stack)
        *(.note.gnu.*)
        *(.comment)
        *(.init_array*)     /* no C++ static constructors in kernel */
        *(.fini_array*)
        *(.ctors)
        *(.dtors)
        *(.gnu.warning)
        *(.dynamic)         /* no dynamic linking in kernel image   */
        *(.dynsym)
        *(.dynstr)
        *(.got.plt)
    }
}
