#include "idt.h"
#include "port.h"
#include "mm.h"
#include "wm.h"
#include "font.h"
#include "panic.h"
#include "log.h"

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

void idt_reload(void) {
    idt_flush((unsigned int)&idtp);
}

void idt_init(void) {
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
    log_printf("[IDT ] exception and IRQ gates installed\n");
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

void isr_handler(struct trap_frame* frame) {
    asm volatile("cli");

    log_write("\n\n====================================================\n");
    log_write("             *** KERNEL EXCEPTION PANIC ***\n");
    log_write("====================================================\n");
    if (frame->int_no < 32) {
        log_printf("Exception : %s\n", exception_names[frame->int_no]);
    } else {
        log_printf("Interrupt Vector: 0x%X\n", frame->int_no);
    }

    log_printf("EIP=0x%X CS=0x%X EFLAGS=0x%X\n",
               frame->eip, frame->cs, frame->eflags);
    log_printf("Error Code=0x%X\n", frame->err_code);
    log_printf("EAX=0x%X EBX=0x%X ECX=0x%X EDX=0x%X\n",
               frame->eax, frame->ebx, frame->ecx, frame->edx);
    log_printf("ESI=0x%X EDI=0x%X EBP=0x%X ESP=0x%X\n",
               frame->esi, frame->edi, frame->ebp, frame->esp_dummy);
    log_printf("CPU Model: %s\n", cpu_model_string);
    log_write("System halted safely. Triple-fault reboot prevented.\n");

    const char* name = (frame->int_no < 32) ? exception_names[frame->int_no] : "Fatal Exception";
    panic_show_exception(frame, name);
}
