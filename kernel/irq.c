#include "irq.h"
#include "idt.h"
#include "port.h"
#include "log.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef struct {
    irq_handler_fn handler;
    void* context;
} irq_entry_t;

static irq_entry_t g_handlers[16][IRQ_MAX_HANDLERS];
static unsigned short g_master_mask = 0xFFFF;
static unsigned short g_slave_mask = 0xFFFF;

static void irq_write_masks(void)
{
    outb(0x21, (unsigned char)g_master_mask);
    outb(0xA1, (unsigned char)g_slave_mask);
}

void irq_init(void)
{
    for (unsigned int irq = 0; irq < 16; irq++) {
        for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
            g_handlers[irq][i].handler = NULL;
            g_handlers[irq][i].context = NULL;
        }
    }
    g_master_mask = 0xFFFF;
    g_slave_mask = 0xFFFF;
    pic_remap();
    irq_write_masks();
    log_printf("[IRQ ] PIC remapped, all IRQs masked during probe\n");
}

int irq_register_handler(unsigned int irq, irq_handler_fn handler,
                         void* context)
{
    if (irq >= 16 || !handler) return -1;
    unsigned int flags = irq_save_flags();
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (g_handlers[irq][i].handler == handler &&
            g_handlers[irq][i].context == context) {
            irq_restore_flags(flags);
            return 0;
        }
    }
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (!g_handlers[irq][i].handler) {
            g_handlers[irq][i].handler = handler;
            g_handlers[irq][i].context = context;
            irq_restore_flags(flags);
            return 0;
        }
    }
    irq_restore_flags(flags);
    log_printf("[IRQ ] handler table full for IRQ%u\n", irq);
    return -2;
}

int irq_unregister_handler(unsigned int irq, irq_handler_fn handler)
{
    if (irq >= 16 || !handler) return -1;
    unsigned int flags = irq_save_flags();
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (g_handlers[irq][i].handler == handler) {
            g_handlers[irq][i].handler = NULL;
            g_handlers[irq][i].context = NULL;
            irq_restore_flags(flags);
            return 0;
        }
    }
    irq_restore_flags(flags);
    return -1;
}

void irq_mask(unsigned int irq)
{
    if (irq >= 16) return;
    unsigned int flags = irq_save_flags();
    if (irq < 8) {
        g_master_mask |= (unsigned short)(1U << irq);
        outb(0x21, (unsigned char)g_master_mask);
    } else {
        g_slave_mask |= (unsigned short)(1U << (irq - 8));
        outb(0xA1, (unsigned char)g_slave_mask);
    }
    irq_restore_flags(flags);
}

void irq_unmask(unsigned int irq)
{
    if (irq >= 16) return;
    unsigned int flags = irq_save_flags();
    if (irq < 8) {
        g_master_mask &= (unsigned short)~(1U << irq);
        outb(0x21, (unsigned char)g_master_mask);
    } else {
        g_slave_mask &= (unsigned short)~(1U << (irq - 8));
        g_master_mask &= (unsigned short)~(1U << 2);
        outb(0xA1, (unsigned char)g_slave_mask);
        outb(0x21, (unsigned char)g_master_mask);
    }
    irq_restore_flags(flags);
}

int irq_is_masked(unsigned int irq)
{
    if (irq >= 16) return 1;
    if (irq < 8) return (g_master_mask & (1U << irq)) != 0;
    return (g_slave_mask & (1U << (irq - 8))) != 0;
}

void irq_mask_all(void)
{
    g_master_mask = 0xFFFF;
    g_slave_mask = 0xFFFF;
    irq_write_masks();
}

void irq_global_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

void irq_global_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

unsigned int irq_save_flags(void)
{
    unsigned int flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void irq_restore_flags(unsigned int flags)
{
    if (flags & 0x200U) {
        __asm__ volatile("sti" ::: "memory");
    } else {
        __asm__ volatile("cli" ::: "memory");
    }
}

int irq_interrupts_enabled(void)
{
    unsigned int flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200U) != 0;
}

void irq_bios_suspend(irq_bios_state_t* state)
{
    if (!state) return;
    state->flags = irq_save_flags();
    state->master_mask = g_master_mask;
    state->slave_mask = g_slave_mask;
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void irq_bios_resume(const irq_bios_state_t* state)
{
    if (!state) return;
    g_master_mask = state->master_mask;
    g_slave_mask = state->slave_mask;
    irq_write_masks();
    irq_restore_flags(state->flags);
}

static void irq_dispatch(unsigned int irq)
{
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        irq_handler_fn handler = g_handlers[irq][i].handler;
        if (handler) handler(irq, g_handlers[irq][i].context);
    }
}

void irq_handler(struct trap_frame* frame)
{
    unsigned int irq = frame->int_no - 32U;
    if (irq >= 16) return;

    if (irq == 7) {
        outb(0x20, 0x0B);
        if (!(inb(0x20) & 0x80)) return;
    } else if (irq == 15) {
        outb(0xA0, 0x0B);
        if (!(inb(0xA0) & 0x80)) {
            outb(0x20, 0x20);
            return;
        }
    }

    irq_dispatch(irq);

    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
