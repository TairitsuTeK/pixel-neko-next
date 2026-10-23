#include "idt.h"
#include "port.h"
#include "mm.h"
#include "wm.h"
#include "font.h"

extern volatile int global_tick;

struct idt_entry {
    unsigned short base_low;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void idt_flush(unsigned int);

// ISR declarations
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

extern void mouse_handle_byte(unsigned char d);
extern void keyboard_handle_byte(unsigned char d);

static void (*isr_table[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
};

static void (*irq_table[16])(void) = {
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
};

static const char* exception_names[32] = {
    "Divide-by-zero Error (#DE)",
    "Debug (#DB)",
    "Non-maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved",
    "x87 FPU Floating-Point Error (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point Exception (#XM)",
    "Virtualization Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Security Exception", "Reserved"
};

static void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00); // Disable all interrupts
    outb(0x3F8 + 3, 0x80); // Enable DLAB
    outb(0x3F8 + 0, 0x03); // 38400 baud (divisor 3)
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(0x3F8 + 2, 0xC7); // FIFO
    outb(0x3F8 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static void serial_putc(char c) {
    int timeout = 50000;
    while (((inb(0x3F8 + 5) & 0x20) == 0) && --timeout);
    outb(0x3F8, c);
}

static void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_puthex32(unsigned int val) {
    const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        serial_putc(hex[(val >> i) & 0xF]);
    }
}

void idt_reload(void) {
    idt_flush((unsigned int)&idtp);
}

void idt_init(void) {
    serial_init();

    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (unsigned int)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // Vectors 0..31: CPU Exceptions
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, (unsigned int)isr_table[i], 0x08, 0x8E);
    }

    // Vectors 32..47: All 16 Hardware IRQs (IRQ0..IRQ15)
    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, (unsigned int)irq_table[i], 0x08, 0x8E);
    }

    pic_remap();

    idt_reload();
    asm volatile("cli");
}

void pic_remap(void) {
    // Remap 8259 PIC
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20); // Master IRQ0-7 -> vectors 0x20-0x27 (32-39)
    outb(0xA1, 0x28); // Slave IRQ8-15 -> vectors 0x28-0x2F (40-47)
    outb(0x21, 0x04); // Master has slave on IRQ2
    outb(0xA1, 0x02); // Slave attached to IRQ2
    outb(0x21, 0x01); // 8086 mode
    outb(0xA1, 0x01);

    // Mask all IRQs on PIC so hardware IRQs never disrupt Real Mode BIOS calls
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void irq_handler(struct trap_frame* frame) {
    if (frame->int_no == 32) {
        // IRQ0: Timer
        global_tick++;
    } else if (frame->int_no == 33 || frame->int_no == 44) {
        // IRQ1 Keyboard or IRQ12 Mouse
        for (;;) {
            unsigned char st = inb(0x64);
            if (!(st & 0x01)) break;
            unsigned char data = inb(0x60);
            if (st & 0x20) mouse_handle_byte(data);
            else keyboard_handle_byte(data);
        }
    }

    // Spurious IRQ7 handling: check Master In-Service Register
    if (frame->int_no == 39) {
        outb(0x20, 0x0B);
        unsigned char isr = inb(0x20);
        if (!(isr & 0x80)) return; // Spurious IRQ7: do not send EOI
    }

    // Spurious IRQ15 handling: check Slave In-Service Register
    if (frame->int_no == 47) {
        outb(0xA0, 0x0B);
        unsigned char isr = inb(0xA0);
        if (!(isr & 0x80)) {
            outb(0x20, 0x20); // Spurious IRQ15: send EOI only to Master
            return;
        }
    }

    // Send EOI to Slave then Master
    if (frame->int_no >= 40) {
        outb(0xA0, 0x20); // EOI to Slave PIC
    }
    outb(0x20, 0x20); // EOI to Master PIC
}

void isr_handler(struct trap_frame* frame) {
    asm volatile("cli");

    serial_puts("\n\n====================================================\n");
    serial_puts("             *** KERNEL EXCEPTION PANIC ***\n");
    serial_puts("====================================================\n");
    if (frame->int_no < 32) {
        serial_puts("Exception : ");
        serial_puts(exception_names[frame->int_no]);
        serial_puts("\n");
    } else {
        serial_puts("Interrupt Vector: ");
        serial_puthex32(frame->int_no);
        serial_puts("\n");
    }

    serial_puts("EIP       : "); serial_puthex32(frame->eip);
    serial_puts("  CS    : "); serial_puthex32(frame->cs);
    serial_puts("  EFLAGS: "); serial_puthex32(frame->eflags);
    serial_puts("\n");

    serial_puts("Error Code: "); serial_puthex32(frame->err_code);
    serial_puts("\n");

    serial_puts("EAX: "); serial_puthex32(frame->eax);
    serial_puts("  EBX: "); serial_puthex32(frame->ebx);
    serial_puts("  ECX: "); serial_puthex32(frame->ecx);
    serial_puts("  EDX: "); serial_puthex32(frame->edx);
    serial_puts("\n");

    serial_puts("ESI: "); serial_puthex32(frame->esi);
    serial_puts("  EDI: "); serial_puthex32(frame->edi);
    serial_puts("  EBP: "); serial_puthex32(frame->ebp);
    serial_puts("  ESP: "); serial_puthex32(frame->esp_dummy);
    serial_puts("\n");

    serial_puts("CPU Model : ");
    serial_puts(cpu_model_string);
    serial_puts("\n");
    serial_puts("System halted safely. Triple-fault reboot prevented.\n");
    // If graphical display is active, render classic system error alert
    if (screen_w >= 320 && screen_h >= 200) {
        int box_w = 340;
        int box_h = 120;
        int bx = (screen_w - box_w) / 2;
        int by = (screen_h - box_h) / 2;

        for (int y = by; y < by + box_h; y++) {
            for (int x = bx; x < bx + box_w; x++) {
                PUT_PIXEL(backbuffer, x, y, 7);
            }
        }
        // Black outline & double frame
        for (int x = bx; x < bx + box_w; x++) {
            PUT_PIXEL(backbuffer, x, by, 0);
            PUT_PIXEL(backbuffer, x, by + box_h - 1, 0);
            PUT_PIXEL(backbuffer, x, by + 2, 0);
            PUT_PIXEL(backbuffer, x, by + box_h - 3, 0);
        }
        for (int y = by; y < by + box_h; y++) {
            PUT_PIXEL(backbuffer, bx, y, 0);
            PUT_PIXEL(backbuffer, bx + box_w - 1, y, 0);
            PUT_PIXEL(backbuffer, bx + 2, y, 0);
            PUT_PIXEL(backbuffer, bx + box_w - 3, y, 0);
        }

        // Draw alert title & details
        font_draw_text(backbuffer, bx + 16, by + 12, "Sorry, a system error occurred.", 0);
        const char* name = (frame->int_no < 32) ? exception_names[frame->int_no] : "Unknown Trap";
        font_draw_text(backbuffer, bx + 16, by + 32, name, 4);

        char eip_str[32] = "EIP: 0x";
        const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 8; i++) {
            eip_str[7 + i] = hex[(frame->eip >> ((7 - i) * 4)) & 0xF];
        }
        eip_str[15] = '\0';
        font_draw_text(backbuffer, bx + 16, by + 52, eip_str, 0);
        font_draw_text(backbuffer, bx + 16, by + 72, cpu_model_string, 8);
        font_draw_text(backbuffer, bx + 16, by + 92, "Press any key to restart...", 1);

        copy_fullscreen_to_lfb(backbuffer);
    }

    // Wait for keypress then reboot
    while (1) {
        if (inb(0x64) & 0x01) {
            inb(0x60);
            outb(0x64, 0xFE);
            outb(0xCF9, 0x06);
        }
        asm volatile("pause");
    }
}
