#ifndef PANIC_H
#define PANIC_H

#include "idt.h"

void panic_show_exception(struct trap_frame* frame, const char* name);
void panic_halt(const char* message);

#endif
