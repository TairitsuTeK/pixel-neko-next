#ifndef TIMER_H
#define TIMER_H

extern volatile int global_tick;

void timer_init(void);
void timer_update(void);
int timer_get_ticks(void);
void sleep_ms(int ms);
void timer_handle_irq(void);
void timer_set_irq_mode(int enabled);
int timer_irq_mode_enabled(void);

#endif // TIMER_H
