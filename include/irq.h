#ifndef IRQ_H
#define IRQ_H

#include "idt.h"

#define IRQ_MAX_HANDLERS 4

typedef void (*irq_handler_fn)(unsigned int irq, void* context);

typedef struct {
    unsigned int flags;
    unsigned short master_mask;
    unsigned short slave_mask;
} irq_bios_state_t;

void irq_init(void);
int irq_register_handler(unsigned int irq, irq_handler_fn handler,
                         void* context);
int irq_unregister_handler(unsigned int irq, irq_handler_fn handler);
void irq_mask(unsigned int irq);
void irq_unmask(unsigned int irq);
int irq_is_masked(unsigned int irq);
void irq_mask_all(void);
void irq_global_enable(void);
void irq_global_disable(void);
unsigned int irq_save_flags(void);
void irq_restore_flags(unsigned int flags);
int irq_interrupts_enabled(void);
void irq_bios_suspend(irq_bios_state_t* state);
void irq_bios_resume(const irq_bios_state_t* state);
void irq_handler(struct trap_frame* frame);

#endif
