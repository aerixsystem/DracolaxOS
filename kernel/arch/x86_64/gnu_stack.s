; gnu_stack.s — marks the kernel stack as non-executable
; This single file satisfies the .note.GNU-stack requirement for the whole
; kernel image. x86_64-elf-ld requires at least one object to declare
; the stack non-executable; without it the linker assumes exec-stack.
section .note.GNU-stack noalloc noexec nowrite progbits
