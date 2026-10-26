#include "timer.h"
#include "port.h"

volatile int global_tick = 0;
static unsigned short g_last_pit;
static unsigned int g_pit_sub_ticks;
static int g_poll_initialized;
static int g_irq_mode;

void timer_init(void)
{
    // Configure PIT Channel 0:
    // Bits 7-6 = 00 (Channel 0)
    // Bits 5-4 = 11 (Access lobyte/hibyte)
    // Bits 3-1 = 010 (Mode 2: Rate Generator)
    // Bit 0    = 0 (16-bit binary)
    outb(0x43, 0x34);
    outb(0x40, (unsigned char)(11932 & 0xFF));
    outb(0x40, (unsigned char)((11932 >> 8) & 0xFF));
    g_poll_initialized = 0;
    g_pit_sub_ticks = 0;
}

void timer_handle_irq(void)
{
    global_tick++;
}

void timer_set_irq_mode(int enabled)
{
    g_irq_mode = enabled != 0;
    g_pit_sub_ticks = 0;
    g_poll_initialized = 0;
}

int timer_irq_mode_enabled(void)
{
    return g_irq_mode;
}

void timer_update(void)
{
    outb(0x43, 0x00); // Latch Counter 0
    unsigned char lo = inb(0x40);
    unsigned char hi = inb(0x40);
    unsigned short count = ((unsigned short)hi << 8) | lo;

    if (!g_poll_initialized) {
        g_last_pit = count;
        g_poll_initialized = 1;
        return;
    }

    // Counter counts downwards from 65535 to 0 at 1.193182 MHz
    unsigned short elapsed;
    if (count <= g_last_pit) {
        elapsed = g_last_pit - count;
    } else {
        elapsed = (unsigned short)((g_last_pit + 65536U) - count);
    }
    g_last_pit = count;

    if (g_irq_mode) return;

    g_pit_sub_ticks += elapsed;
    // 100 Hz: 11932 PIT counts = 10.0 ms
    while (g_pit_sub_ticks >= 11932) {
        g_pit_sub_ticks -= 11932;
        global_tick++;
    }
}

int timer_get_ticks(void)
{
    timer_update();
    return global_tick;
}

void sleep_ms(int ms)
{
    if (ms <= 0) return;
    int target_ticks = ms / 10;
    if (target_ticks <= 0) target_ticks = 1;
    
    timer_update();
    int start_tick = global_tick;
    while ((global_tick - start_tick) < target_ticks) {
        timer_update();
        for (volatile int i = 0; i < 200; i++);
    }
}
