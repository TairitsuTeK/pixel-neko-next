#ifndef TIMER_H
#define TIMER_H

extern volatile int global_tick;

void timer_init(void);
void timer_update(void);
int timer_get_ticks(void);
void sleep_ms(int ms);

#endif // TIMER_H
