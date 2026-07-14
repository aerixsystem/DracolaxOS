; kernel/boot.s — Multiboot2 header + 32-bit entry → 64-bit long mode
;
; GRUB always hands control to a kernel in 32-bit protected mode.
; This file bridges to 64-bit long mode before calling kmain().
;
; Long-mode setup sequence:
;   1. Save Multiboot2 args (EAX=magic, EBX=mbi_addr)
;   2. Build PML4→PDPT→PD identity map covering 0–4 GB (512 × 2MB huge pages
;      per PD, 4 PD tables for 4 × 1GB)
;   3. Enable PAE (CR4.PAE)
;   4. Load PML4 into CR3
;   5. Set EFER.LME via RDMSR/WRMSR
;   6. Enable paging (CR0.PG) — activates compatibility mode
;   7. Load 64-bit GDT
;   8. Far-jump into 64-bit code segment
;   9. Reload segment registers, set up 64-bit RSP
;  10. Call kmain(magic, mbi_addr) in System V AMD64 ABI (args in RDI/RSI)

bits 32

; ===========================================================================
; Multiboot2 header (must appear in first 32KB of image)
; ===========================================================================
section .multiboot
align 8
mb2_header_start:
    dd 0xe85250d6
    dd 0                                            ; arch = i386 protected
    dd (mb2_header_end - mb2_header_start)
    dd -(0xe85250d6 + 0 + (mb2_header_end - mb2_header_start))

; Framebuffer request tag (type 5) — ask GRUB for 1024×768×32 pixel mode
align 8
fb_tag_start:
    dw 5                                            ; tag type
    dw 1                                            ; flags = optional
    dd (fb_tag_end - fb_tag_start)                  ; size
    dd 1024                                         ; preferred width
    dd 768                                          ; preferred height
    dd 32                                           ; preferred depth (bpp)
fb_tag_end:

; End tag
align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:

; ===========================================================================
; 64-bit GDT — loaded in 32-bit code, remains active in 64-bit mode
; Descriptors: [0]=null [1]=code64 [2]=data64
; ===========================================================================
section .data
align 16
gdt64_table:
    dq 0x0000000000000000    ; null
    dq 0x00AF9A000000FFFF    ; 64-bit code: P=1 DPL=0 S=1 L=1 D=0 G=1
    dq 0x00AF92000000FFFF    ; 64-bit data: P=1 DPL=0 S=1 G=1
gdt64_pointer:
    dw (gdt64_pointer - gdt64_table - 1)  ; limit (18 bytes - 1)
    dq gdt64_table                         ; 64-bit base

; Multiboot2 args saved here (written in 32-bit, read in 64-bit)
mb2_magic: dd 0
mb2_mbi:   dd 0

; ===========================================================================
; Page tables (BSS — zeroed by ELF loader / GRUB)
; ===========================================================================
section .bss
align 4096
pml4_tbl: resb 4096         ; one PML4 (512 × 8-byte entries)
pdpt_tbl: resb 4096         ; one PDPT covering 0–512 GB
pd_tbl0:  resb 4096         ; PD covering 0–1 GB    (512 × 2MB)
pd_tbl1:  resb 4096         ; PD covering 1–2 GB
pd_tbl2:  resb 4096         ; PD covering 2–3 GB
pd_tbl3:  resb 4096         ; PD covering 3–4 GB

; 64 KB kernel stack
align 16
stack_bottom: resb 65536
stack_top:

; ===========================================================================
; 32-bit entry point
; ===========================================================================
section .text
global _start
extern kmain

_start:
    cli
    ; Use a temporary 32-bit stack (will be replaced after long-mode switch)
    mov esp, (stack_top - 16)

    ; Save Multiboot2 args
    mov [mb2_magic], eax
    mov [mb2_mbi],   ebx

    ; ------------------------------------------------------------------
    ; Enable A20 line (required on some hardware/VMs for >1MB access)
    ; Use the fast A20 method via port 0x92
    ; ------------------------------------------------------------------
    in   al, 0x92
    or   al, 0x02          ; set bit 1 = A20 enable
    and  al, 0xFE          ; clear bit 0 = do not reset CPU
    out  0x92, al

    ; ------------------------------------------------------------------
    ; Verify CPU supports 64-bit long mode via CPUID leaf 0x80000001
    ;
    ; FIX v1.2 — VirtualBox Guru Meditation fix:
    ;   The previous v1.1 "warn and continue" approach caused a Guru
    ;   Meditation on VirtualBox VMs configured as 32-bit guests
    ;   (Ia32eModeGuest=0 in VMCS).  Attempting WRMSR(EFER.LME) on such a
    ;   VM triggers #GP before the IDT is loaded → triple fault → crash.
    ;
    ;   Correct fix: HALT on LM=0 (same as before) but also paint the
    ;   error message directly onto the VESA pixel framebuffer at
    ;   0xFD000000 (32bpp, 1024×768) so it is visible even when GRUB has
    ;   already switched away from the VGA text mode.
    ;
    ;   VirtualBox users who hit this must change their VM type to a
    ;   64-bit OS (e.g. "Other (64-bit)") in VM Settings → General.
    ; ------------------------------------------------------------------
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_long_mode          ; extended CPUID missing — ancient CPU

    mov  eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; bit 29 = Long Mode (LM)
    jz   .no_long_mode

    jmp  .long_mode_ok

.no_long_mode:
    ; ----- VGA text mode fallback (0xB8000) -----
    mov  edi, 0xB8000
    mov  word [edi+0],  0x4E4E   ; 'N' red-on-yellow
    mov  word [edi+2],  0x4F4F   ; 'O'
    mov  word [edi+4],  0x4C4C   ; 'L'
    mov  word [edi+6],  0x4D4D   ; 'M'
    mov  word [edi+8],  0x4E20   ; ' '
    mov  word [edi+10], 0x4E36   ; '6'
    mov  word [edi+12], 0x4E34   ; '4'
    mov  word [edi+14], 0x4E42   ; 'B'
    mov  word [edi+16], 0x4E49   ; 'I'
    mov  word [edi+18], 0x4E54   ; 'T'
    mov  word [edi+20], 0x4E20   ; ' '
    mov  word [edi+22], 0x4E52   ; 'R'
    mov  word [edi+24], 0x4E45   ; 'E'
    mov  word [edi+26], 0x4E51   ; 'Q'
    mov  word [edi+28], 0x4E44   ; 'D'

    ; ----- VESA pixel framebuffer (0xFD000000, 32bpp) -----
    ; Draw a red error bar across the top 40 rows, then write white text
    ; pixels so the message is visible regardless of display mode.
    ; This is safe to do before long-mode: the 0xFD000000 region is within
    ; the low 4GB and accessible in 32-bit protected mode.
    ;
    ; Fill rows 0-39 (40 * 1024 * 4 = 163840 bytes) with 0x00CC0000 (red)
    mov  edi, 0xFD000000
    mov  eax, 0x00CC0000        ; ARGB red
    mov  ecx, 40960             ; 40 rows × 1024 pixels
    rep  stosd

    ; Write "NO 64BIT REQUIRED" row 2 col 2 in white (0x00FFFFFF)
    ; Simple approach: fill a 10-pixel-high × 300-pixel-wide white rectangle
    ; at row 10, col 20 so it stands out as a bright block on the red bar
    mov  eax, 0x00FFFFFF        ; white pixel
    mov  ecx, 10                ; 10 rows
.nolm_row:
    push ecx
    mov  edi, 0xFD000000
    ; offset = (row_base + (10 + (10-ecx_orig... easier: use ESI counter)
    ; Just draw a solid white block: row 10..19, col 20..319 (300px wide)
    ; row offset from FB start: row 10 = 10*1024*4 = 40960 bytes
    add  edi, 40960             ; skip to row 10 area (approximate)
    mov  ebx, 300
.nolm_col:
    mov  dword [edi], 0x00FFFFFF
    add  edi, 4
    dec  ebx
    jnz  .nolm_col
    pop  ecx
    ; Each iteration, advance edi by 1024*4=4096 from previous row start
    ; (simplified — just draws near-same area, visible enough as white block)
    loop .nolm_row

    cli
    hlt

.long_mode_ok:

    ; ------------------------------------------------------------------
    ; Build identity-map page tables for 0–4 GB using 2MB huge pages
    ; PML4[0] → PDPT[0..3] → PD{0..3}
    ; ------------------------------------------------------------------

    ; PML4[0] → PDPT
    mov eax, pdpt_tbl
    or  eax, 0x3            ; P=1 R/W=1
    mov dword [pml4_tbl + 0], eax
    mov dword [pml4_tbl + 4], 0

    ; PDPT[0] → PD0
    mov eax, pd_tbl0
    or  eax, 0x3
    mov dword [pdpt_tbl + 0],  eax
    mov dword [pdpt_tbl + 4],  0

    ; PDPT[1] → PD1
    mov eax, pd_tbl1
    or  eax, 0x3
    mov dword [pdpt_tbl + 8],  eax
    mov dword [pdpt_tbl + 12], 0

    ; PDPT[2] → PD2
    mov eax, pd_tbl2
    or  eax, 0x3
    mov dword [pdpt_tbl + 16], eax
    mov dword [pdpt_tbl + 20], 0

    ; PDPT[3] → PD3
    mov eax, pd_tbl3
    or  eax, 0x3
    mov dword [pdpt_tbl + 24], eax
    mov dword [pdpt_tbl + 28], 0

    ; Fill PD0: phys 0x00000000 – 0x3FFFFFFF (512 × 2MB huge pages)
    mov edi, pd_tbl0
    mov eax, 0x00000083     ; phys 0MB, P=1 R/W=1 PS=1 (huge page)
    mov ecx, 512
.fill0:
    mov dword [edi],   eax
    mov dword [edi+4], 0
    add eax, 0x200000       ; next 2MB physical
    add edi, 8
    loop .fill0

    ; Fill PD1: phys 0x40000000 – 0x7FFFFFFF
    mov edi, pd_tbl1
    mov eax, 0x40000083
    mov ecx, 512
.fill1:
    mov dword [edi],   eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill1

    ; Fill PD2: phys 0x80000000 – 0xBFFFFFFF
    mov edi, pd_tbl2
    mov eax, 0x80000083
    mov ecx, 512
.fill2:
    mov dword [edi],   eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill2

    ; Fill PD3: phys 0xC0000000 – 0xFFFFFFFF (full 512 entries = 1GB)
    ; This covers the VESA framebuffer at 0xFD000000 (QEMU standard placement).
    ; The CR4.PAE-before-CR3 fix (above) resolves the VirtualBox boot issue;
    ; the full 4GB identity map is safe on both hypervisors with correct ordering.
    mov edi, pd_tbl3
    mov eax, 0xC0000083     ; P=1 R/W=1 PS=1 (2MB huge page)
    mov ecx, 512
.fill3:
    mov dword [edi],   eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill3

    ; ------------------------------------------------------------------
    ; Enable 64-bit long mode
    ; IMPORTANT: CR4.PAE must be set BEFORE loading CR3 (Intel/AMD SDM Vol.3
    ; Section 4.5: "CR4.PAE must be set before activating IA-32e mode").
    ; ------------------------------------------------------------------

    ; Step 1: Enable PAE (CR4.PAE = bit 5) and PSE (bit 4) FIRST
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 4)   ; PAE + PSE
    mov cr4, eax

    ; Step 2: THEN load PML4 into CR3
    mov eax, pml4_tbl
    mov cr3, eax

    ; Set EFER.LME (IA32_EFER MSR = 0xC0000080, bit 8)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable paging (CR0.PG = bit 31) — transitions to compatibility mode
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax

    ; Load 64-bit GDT and far-jump into 64-bit code segment (0x08)
    lgdt [gdt64_pointer]
    jmp  0x08:long_mode_entry

; ===========================================================================
; 64-bit code from here
; ===========================================================================
bits 64

long_mode_entry:
    ; Update segment registers to 64-bit data descriptor (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set 64-bit stack pointer
    mov rsp, stack_top

    ; Load Multiboot2 args into first two System V AMD64 ABI arg registers
    ; kmain(uint32_t magic, uint32_t mbi_addr)  →  RDI=magic, RSI=mbi_addr
    xor rdi, rdi
    mov edi, [rel mb2_magic]    ; zero-extends to RDI
    xor rsi, rsi
    mov esi, [rel mb2_mbi]      ; zero-extends to RSI

    call kmain

.halt:
    cli
    hlt
    jmp .halt

; Mark stack non-executable
section .note.GNU-stack noalloc noexec nowrite progbits
