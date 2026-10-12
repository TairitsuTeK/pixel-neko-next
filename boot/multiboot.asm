bits 32
global _start
extern kernel_main

section .multiboot
align 4
    dd 0x1BADB002
    dd 0x00000003
    dd -(0x1BADB002 + 0x00000003)

section .text
_start:
    cli
    
    mov [0x7F60], dl
    mov [0x7F64], ebx

    lgdt [gdt_ptr]
    jmp 0x08:.reload_cs

.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Enable CPU L1/L2 cache: clear CR0.CD (bit 30) and CR0.NW (bit 29)
    mov eax, cr0
    and eax, 0x9FFFFFFF
    mov cr0, eax
    wbinvd
    
    mov esp, 0x90000

    call kernel_main
    
.hang:
    cli
    hlt
    jmp .hang

section .data
align 4
gdt_start:
    dd 0, 0
    dd 0x0000FFFF, 0x00CF9A00 ; 0x08: 32-bit code
    dd 0x0000FFFF, 0x00CF9200 ; 0x10: 32-bit data
    dd 0x0000FFFF, 0x00009A00 ; 0x18: 16-bit code
    dd 0x0000FFFF, 0x00009200 ; 0x20: 16-bit data
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start