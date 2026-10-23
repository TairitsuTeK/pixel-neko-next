#include "speaker.h"
#include "port.h"
#include "timer.h"

void speaker_init(void)
{
    // Ensure speaker gate and data are turned off
    outb(0x61, inb(0x61) & 0xFC);
}

void speaker_play(unsigned int freq, unsigned int duration_ms)
{
    if (freq < 20 || freq > 20000) return;
    
    unsigned int div = 1193182 / freq;
    outb(0x43, 0xB6); // Channel 2, lobyte/hibyte, Mode 3, binary
    outb(0x42, (unsigned char)(div & 0xFF));
    outb(0x42, (unsigned char)((div >> 8) & 0xFF));
    
    unsigned char tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, tmp | 3); // enable speaker
    }
    
    sleep_ms(duration_ms);
    
    // Turn speaker off
    outb(0x61, inb(0x61) & 0xFC);
}

void speaker_boot_chord(void)
{
    speaker_play(523, 60); // C5
    speaker_play(659, 60); // E5
    speaker_play(784, 60); // G5
    speaker_play(1046, 120); // C6
}

void speaker_beep(void)
{
    speaker_play(880, 50); // A5 standard beep
}
