bits 32
global vbe_set_mode
global vbe_get_mode_info
global vbe_get_controller_info
global bios_set_mode
global bios_shutdown
global bios_set_dac_ramp

section .text

vbe_set_mode:
    push ebp
    mov ebp, esp
    pushad
    
    mov bx, [ebp+8]
    mov [0x7FF0], bx
    mov dword [0x7FF4], 0x4F02
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

vbe_get_mode_info:
    push ebp
    mov ebp, esp
    pushad
    
    mov cx, [ebp+8]
    mov [0x7FF0], cx
    mov dword [0x7FF4], 0x4F01
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

vbe_get_controller_info:
    push ebp
    mov ebp, esp
    pushad
    
    mov dword [0x7FF4], 0x4F00
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

bios_set_mode:
    push ebp
    mov ebp, esp
    pushad
    
    mov ax, [ebp+8]
    mov [0x7FF0], ax
    mov dword [0x7FF4], 0x0000
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

bios_shutdown:
    push ebp
    mov ebp, esp
    pushad
    
    mov dword [0x7FF4], 0x5307
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

bios_set_dac_ramp:
    push ebp
    mov ebp, esp
    pushad
    
    mov dword [0x7FF4], 0x4F09
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

align 4
payload_start:
bits 16
    mov ax, 0x20 
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    mov eax, cr0
    and eax, ~1
    mov cr0, eax
    
    jmp 0x00:real_mode - payload_start + 0x8000

real_mode:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov sp, 0x7E00
    
    lidt [ivt_ptr - payload_start + 0x8000]
    
    mov ax, [0x7FF4]
    cmp ax, 0x0000
    je .do_bios
    cmp ax, 0x4F00
    je .do_controller
    cmp ax, 0x4F02
    je .do_set
    cmp ax, 0x4F01
    je .do_get
    cmp ax, 0x4F09
    je .do_set_ramp
    cmp ax, 0x5307
    je .do_shutdown
    jmp .done

.do_controller:
    mov di, 0x8800
    mov byte [di], 'V'
    mov byte [di+1], 'B'
    mov byte [di+2], 'E'
    mov byte [di+3], '2'
    mov ax, 0x4F00
    int 0x10
    jmp .done

.do_bios:
    mov ax, [0x7FF0]
    int 0x10
    jmp .done

.do_set:
    mov bx, [0x7FF0]
    int 0x10
    jmp .done

.do_get:
    mov cx, [0x7FF0]
    mov di, 0x9000
    int 0x10
    jmp .done

.do_set_ramp:
    ; Build 256-entry linear identity ramp table at 0x1000:
    ; Format for VBE 4F09h (Set Palette Data): Blue, Green, Red, Alignment
    xor bx, bx
    mov di, 0x1000
.build_4f09:
    mov al, bl
    mov [di], al
    mov [di+1], al
    mov [di+2], al
    mov byte [di+3], 0
    add di, 4
    inc bl
    jnz .build_4f09

    mov ax, 0x4F09
    xor bx, bx
    mov cx, 256
    xor dx, dx
    mov di, 0x1000
    int 0x10

    ; Also try standard VGA BIOS AX=1012h (Set Block of DAC Registers)
    ; Format: Red, Green, Blue (6-bit values 0..63)
    xor bx, bx
    mov di, 0x1400
.build_1012:
    mov al, bl
    shr al, 2
    mov [di], al
    mov [di+1], al
    mov [di+2], al
    add di, 3
    inc bl
    jnz .build_1012

    mov ax, 0x1012
    xor bx, bx
    mov cx, 256
    mov dx, 0x1400
    int 0x10
    jmp .done

.do_shutdown:
    ; Disconnect any previous APM connection
    mov ax, 0x5304
    xor bx, bx
    int 0x15

    ; Connect real mode APM interface
    mov ax, 0x5301
    xor bx, bx
    int 0x15

    ; APM Driver Version 1.2
    mov ax, 0x530E
    xor bx, bx
    mov cx, 0x0102
    int 0x15

    ; Enable power management for all devices
    mov ax, 0x5308
    mov bx, 0x0001
    mov cx, 0x0001
    int 0x15

    ; Engage power management for all devices
    mov ax, 0x530F
    mov bx, 0x0001
    mov cx, 0x0001
    int 0x15

    ; Set Power State: Off (BX=0001h all devices, CX=0003h Off)
    mov ax, 0x5307
    mov bx, 0x0001
    mov cx, 0x0003
    int 0x15

    ; If APM fails, try Bochs / QEMU / VirtualBox ports
    mov dx, 0x604
    mov ax, 0x2000
    out dx, ax
    mov dx, 0xB004
    out dx, ax
    mov dx, 0x4004
    mov ax, 0x3400
    out dx, ax
    jmp .done

.done:
    mov [0x7FF8], ax

    mov eax, cr0
    or eax, 1
    and eax, 0x9FFFFFFF ; Clear CR0.CD (bit 30) and CR0.NW (bit 29) to ensure cache is enabled
    mov cr0, eax
    wbinvd
    
    jmp 0x08:pm32 - payload_start + 0x8000
    
bits 32
pm32:
    mov ax, 0x10 
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    mov esp, [0x7FFC]
    
    popad
    movzx eax, word [0x7FF8]
    pop ebp
    ret

align 4
ivt_ptr:
    dw 0x03FF
    dd 0x00000000

payload_end:
