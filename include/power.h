#ifndef POWER_H
#define POWER_H

void sys_reboot(void) __attribute__((noreturn));
void sys_shutdown(void) __attribute__((noreturn));

#endif
