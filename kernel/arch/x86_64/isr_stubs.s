; kernel/isr_stubs.s — 64-bit interrupt service routine stubs
;
; In x86_64, the CPU ALWAYS pushes 5 qwords on interrupt (no privilege-change
; difference for SS:RSP — they are always pushed):
;   [RSP+0]  RIP
;   [RSP+8]  CS
;   [RSP+16] RFLAGS
;   [RSP+24] RSP (saved)
;   [RSP+32] SS
; For exceptions with an error code the CPU additionally pushes ERROR_CODE
; BEFORE RIP (i.e. at the lowest address).
;
; Stack frame delivered to isr_dispatch(struct isr_frame *):
;   [RSP+  0]  r15
;   [RSP+  8]  r14
;   [RSP+ 16]  r13
;   [RSP+ 24]  r12
;   [RSP+ 32]  r11
;   [RSP+ 40]  r10
;   [RSP+ 48]  r9
;   [RSP+ 56]  r8
;   [RSP+ 64]  rbp
;   [RSP+ 72]  rdi   (value at time of interrupt — saved before overwrite)
;   [RSP+ 80]  rsi
;   [RSP+ 88]  rdx
;   [RSP+ 96]  rcx
;   [RSP+104]  rbx
;   [RSP+112]  rax
;   [RSP+120]  int_no
;   [RSP+128]  err_code
;   [RSP+136]  rip   (CPU-pushed)
;   [RSP+144]  cs    (CPU-pushed)
;   [RSP+152]  rflags(CPU-pushed)
;   [RSP+160]  rsp   (CPU-pushed)
;   [RSP+168]  ss    (CPU-pushed)

bits 64
extern isr_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0     ; fake error code
    push qword %1    ; int_no
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; CPU already pushed error code (8 bytes)
    push qword %1    ; int_no
    jmp  isr_common
%endmacro

; CPU exceptions 0-31
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQs 32-47
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; Syscall INT 0x80
ISR_NOERR 128

; Common entry: save all registers, dispatch to C, restore, return
;
; ALIGNMENT FIX (audit 3.11):
; The System V AMD64 ABI requires RSP % 16 == 0 at a CALL site (i.e. RSP
; must be 8-byte offset from 16-byte boundary just before the call, because
; CALL itself pushes the 8-byte return address).
;
; When the CPU takes an interrupt it pushes 5 qwords (40 bytes) for no-error
; vectors, or 6 qwords (48 bytes) for error-code vectors.  Our stubs then
; push int_no + (fake) err_code = 16 more bytes.  The isr_common block below
; pushes 15 more registers (120 bytes).  Total pushed before CALL isr_dispatch:
;   no-error:    40 + 16 + 120 = 176 bytes  (176 % 16 == 0 → already aligned)
;   error-code:  48 + 16 + 120 = 184 bytes  (184 % 16 == 8 → misaligned)
;
; To guarantee 16-byte alignment at the CALL regardless of vector type, we
; AND RSP down to a 16-byte boundary and save the original RSP for restore.
; This is the standard approach used by Linux (entry_64.S) and LLVM.
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi           ; save original RDI before we overwrite it
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Align RSP to 16 bytes before calling C.  Save unaligned RSP in RBX
    ; (already saved above) — we restore RSP explicitly after the call.
    mov  rbx, rsp          ; rbx = unaligned frame pointer (also saved in frame)
    and  rsp, -16        ; align down to 16-byte boundary

    mov  rdi, rbx          ; first arg = &isr_frame (unaligned ptr is fine for C)
    call isr_dispatch

    mov  rsp, rbx          ; restore exact pre-alignment RSP

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16       ; discard int_no + err_code
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
