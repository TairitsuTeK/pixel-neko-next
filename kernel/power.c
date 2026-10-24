#include "power.h"
#include "port.h"
#include "wm.h"
#include "font.h"
#include "disk.h"
#include "speaker.h"
#include "audio.h"

extern void bios_shutdown(void);

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void sys_reboot(void) {
    // 0. Stop and silence audio engine and sound card DMA
    audio_stop();

    // Play reboot audio melody before reset
    speaker_reboot_chord();

    // Flush any pending disk writes
    disk_flush_all();

    // 1. Keyboard Controller reset pulse
    outb(0x64, 0xFE);

    // 2. PCI reset vector
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);

    // 3. Triple fault fallback
    struct {
        unsigned short limit;
        unsigned int base;
    } __attribute__((packed)) null_idtp = {0, 0};
    asm volatile("lidt %0; int3" : : "m"(null_idtp));

    for (;;) {
        asm volatile("cli; hlt");
    }
}

static void draw_safe_to_turn_off_screen(void) {
    // Solid black screen
    for (int y = 0; y < screen_h; y++) {
        for (int x = 0; x < screen_w; x++) {
            PUT_PIXEL(backbuffer, x, y, 0);
        }
    }

    const char* line1 = "It is now safe to turn off your computer.";
    const char* line2 = "Pixel Neko Next";
    
    int len1 = my_strlen(line1);
    int len2 = my_strlen(line2);

    int cy = screen_h / 2;
    int cx1 = (screen_w - len1 * 8) / 2;
    int cx2 = (screen_w - len2 * 8) / 2;

    // Vintage warm orange / gold text (Palette color 14: yellow/gold, 12: bright orange)
    // Draw decorative border lines
    int bx1 = (screen_w - 420) / 2;
    int bx2 = bx1 + 420;
    for (int x = bx1; x < bx2; x++) {
        PUT_PIXEL(backbuffer, x, cy - 35, 14);
        PUT_PIXEL(backbuffer, x, cy + 45, 14);
    }

    font_draw_text(backbuffer, cx1, cy - 10, line1, 14);
    font_draw_text(backbuffer, cx2, cy + 15, line2, 15);

    copy_fullscreen_to_lfb(backbuffer);
}

void sys_shutdown(void) {
    // 0. Stop and silence audio engine and sound card DMA
    audio_stop();

    // Play shutdown audio melody before powering off
    speaker_shutdown_chord();

    // 1. Flush disk caches
    disk_flush_all();

    // 2. Attempt real mode BIOS APM shutdown
    bios_shutdown();

    // 3. Simulator I/O shutdown ports (QEMU / Bochs / VirtualBox)
    outw(0x604, 0x2000);   // QEMU
    outw(0xB004, 0x2000);  // Bochs
    outw(0x4004, 0x3400);  // VirtualBox

    // 4. If control reaches here, the machine has an AT mechanical power supply (e.g. ASUS KN97)
    // Display the classic full screen "It is now safe to turn off your computer."
    draw_safe_to_turn_off_screen();

    // 5. Permanently halt CPU with interrupts disabled
    asm volatile(
        "cli\n"
        "1:\n"
        "hlt\n"
        "jmp 1b\n"
    );

    for (;;) {}
}
