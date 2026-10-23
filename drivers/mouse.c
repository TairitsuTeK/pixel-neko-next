// mouse.c
#include "port.h"
#include "wm.h"

static int mx = 160, my = 100;
static int btn = 0;
static int cycle = 0;
static unsigned char buf[3];
static int init_done = 1;

static unsigned short saved[16][16];
static int saved_x = -1, saved_y = -1;

int mouse_speed = 2;

void mouse_set_speed(int s) { if(s>=1 && s<=5) mouse_speed = s; }
int mouse_get_speed(void) { return mouse_speed; }

static inline void kbc_wait_write(void) {
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout);
}

static inline int kbc_wait_read(void) {
    int timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout);
    return timeout > 0;
}

void mouse_init(void)
{
    for (volatile int i = 0; i < 100000; i++);
    
    // Drain any leftover data from controller
    for (int i = 0; i < 100; i++) {
        if (inb(0x64) & 0x01) inb(0x60);
        for (volatile int j = 0; j < 1000; j++);
    }
    
    // Enable second PS/2 port (auxiliary device)
    kbc_wait_write();
    outb(0x64, 0xA8);
    for (volatile int i = 0; i < 50000; i++);

    // Read controller configuration byte
    kbc_wait_write();
    outb(0x64, 0x20);
    for (volatile int i = 0; i < 50000; i++);
    if (!kbc_wait_read()) return;
    unsigned char cfg = inb(0x60);

    // Enable mouse IRQ12 (bit 1) and enable mouse clock (clear bit 5)
    cfg |= 0x02;
    cfg &= ~0x20;

    // Write back controller configuration byte
    kbc_wait_write();
    outb(0x64, 0x60);
    for (volatile int i = 0; i < 50000; i++);
    kbc_wait_write();
    outb(0x60, cfg);
    for (volatile int i = 0; i < 50000; i++);

    // Tell mouse to use default settings
    kbc_wait_write();
    outb(0x64, 0xD4);
    for (volatile int i = 0; i < 50000; i++);
    kbc_wait_write();
    outb(0x60, 0xF6); // Set defaults
    for (volatile int i = 0; i < 50000; i++);
    for (int t = 0; t < 100000; t++) {
        if (inb(0x64) & 0x01) { inb(0x60); break; }
    }

    // Enable mouse streaming
    kbc_wait_write();
    outb(0x64, 0xD4);
    for (volatile int i = 0; i < 50000; i++);
    kbc_wait_write();
    outb(0x60, 0xF4);
    for (volatile int i = 0; i < 50000; i++);
    for (int t = 0; t < 100000; t++) {
        if (inb(0x64) & 0x01) { inb(0x60); break; }
    }

    // Drain any remaining ACKs or bytes
    for (int i = 0; i < 50; i++) {
        if (inb(0x64) & 0x01) inb(0x60);
        for (volatile int j = 0; j < 500; j++);
    }

    cycle = 0;
    init_done = 1;
}

void mouse_handle_byte(unsigned char d)
{
    if (cycle == 0) {
        // Bit 3 of byte 0 in a PS/2 packet MUST be 1.
        // Also bits 6 and 7 (overflow) should be 0.
        if (!(d & 0x08) || (d & 0xC0)) return;
    }
    buf[cycle++] = d;
    if (cycle == 3) {
        cycle = 0;
        btn = buf[0] & 0x07;
        int dx = buf[1];
        int dy = buf[2];
        if (buf[0] & 0x10) dx = dx - 256;
        if (buf[0] & 0x20) dy = dy - 256;
        mx += dx * mouse_speed / 2;
        my -= dy * mouse_speed / 2; // Y axis is inverted
        if (mx < 0) mx = 0;
        if (mx > screen_w - 1) mx = screen_w - 1;
        if (my < 0) my = 0;
        if (my > screen_h - 1) my = screen_h - 1;
    }
}

void mouse_get(int* x, int* y, int* b)
{
    if (x) *x = mx;
    if (y) *y = my;
    if (b) *b = btn;
}

void mouse_draw(unsigned char* vga)
{
    if (!init_done) return;
    if (saved_x >= 0) {
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                if (saved_y+i < screen_h && saved_x+j < screen_w) {
                    int offset = (saved_y+i) * screen_pitch + (saved_x+j) * (screen_bpp > 8 ? 2 : 1);
                    if(screen_bpp > 8) *((unsigned short*)&vga[offset]) = saved[i][j]; else vga[offset] = (unsigned char)saved[i][j];
                }
    }
    int x = mx, y = my;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) {
            if (y+i < screen_h && x+j < screen_w) {
                int offset = (y+i) * screen_pitch + (x+j) * (screen_bpp > 8 ? 2 : 1);
                saved[i][j] = screen_bpp > 8 ? *((unsigned short*)&vga[offset]) : vga[offset];
            } else {
                saved[i][j] = 0;
            }
        }
    saved_x = x;
    saved_y = y;
    
    static const char* cursor_shape[16] = {
        "X               ",
        "XX              ",
        "X.X             ",
        "X..X            ",
        "X...X           ",
        "X....X          ",
        "X.....X         ",
        "X......X        ",
        "X.......X       ",
        "X........X      ",
        "X.........X     ",
        "X......XXXX     ",
        "X..X..X         ",
        "X.X X..X        ",
        "XX  X..X        ",
        "     XX         "
    };

    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            if (cursor_shape[i][j] != ' ') {
                int px = x + j, py = y + i;
                if (px < screen_w && py < screen_h) {
                    PUT_PIXEL(vga, px, py, cursor_shape[i][j] == 'X' ? 15 : 0);
                }
            }
        }
    }
}

void mouse_reset(unsigned char* vga)
{
    if (saved_x >= 0) {
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                if (saved_y+i < screen_h && saved_x+j < screen_w) {
                    int offset = (saved_y+i) * screen_pitch + (saved_x+j) * (screen_bpp > 8 ? 2 : 1);
                    if(screen_bpp > 8) *((unsigned short*)&vga[offset]) = saved[i][j]; else vga[offset] = (unsigned char)saved[i][j];
                }
    }
    saved_x = -1;
    saved_y = -1;
}