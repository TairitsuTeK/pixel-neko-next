// mouse.c
#include "port.h"
#include "wm.h"
#include "irq.h"
#include "log.h"

static int mx = 160, my = 100;
static int btn = 0;
static int cycle = 0;
static unsigned char buf[3];
static int init_done = 1;

static unsigned int saved[16][16];
static int saved_x = -1, saved_y = -1;

int mouse_speed = 2;

void mouse_set_speed(int s) { if(s>=1 && s<=5) mouse_speed = s; }
int mouse_get_speed(void) { return mouse_speed; }

/* ---- 8042 keyboard controller helpers -------------------------------
 * Every wait is bounded: firmware that emulates the PS/2 ports through SMM
 * (CSM boards with "Legacy USB Support") may never acknowledge a command, and
 * the desktop still has to come up in that case. */
#define KBC_TIMEOUT 100000

static int kbc_wait_write(void)
{
    for (int i = 0; i < KBC_TIMEOUT; i++) {
        if (!(inb(0x64) & 0x02)) return 1;
    }
    return 0;
}

static int kbc_wait_read(void)
{
    for (int i = 0; i < KBC_TIMEOUT; i++) {
        if (inb(0x64) & 0x01) return 1;
    }
    return 0;
}

/* Discard whatever the firmware left in the output buffer. */
static void kbc_drain(void)
{
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
        for (volatile int j = 0; j < 100; j++);
    }
}

static int kbc_cmd(unsigned char cmd)
{
    if (!kbc_wait_write()) return 0;
    outb(0x64, cmd);
    return 1;
}

static int kbc_write(unsigned char data)
{
    if (!kbc_wait_write()) return 0;
    outb(0x60, data);
    return 1;
}

static int kbc_read(unsigned char* out)
{
    if (!kbc_wait_read()) return 0;
    *out = inb(0x60);
    return 1;
}

/* Send one byte to the auxiliary (mouse) port and expect its ACK. */
static int mouse_cmd(unsigned char cmd)
{
    unsigned char ack;
    if (!kbc_cmd(0xD4)) return 0;
    if (!kbc_write(cmd)) return 0;
    if (!kbc_read(&ack)) return 0;
    return ack == 0xFA;
}

static void kbc_settle(void)
{
    for (volatile int i = 0; i < 20000; i++);
    kbc_drain();
}

void mouse_init(void)
{
    /* 0 = never probed, 1 = controller configured, -1 = leave firmware alone */
    static int controller_state = 0;

    /* Called once from kernel_main before IRQ1/IRQ12 are unmasked. Re-entry is
     * a no-op; the 8042 must not be reprogrammed after the desktop starts. */
    if (controller_state != 0) {
        cycle = 0;
        kbc_drain();
        log_printf("[PS2 ] mouse already set up, controller left untouched\n");
        return;
    }

    kbc_drain();

    /* Read the controller command byte (0x20). A real i8042 answers with a
     * value whose reserved bit 7 is clear; anything else means the byte we
     * drained was a queued keystroke or the port is firmware-emulated. */
    unsigned char cfg = 0;
    int have_cfg = 0;
    if (kbc_cmd(0x20) && kbc_read(&cfg) && !(cfg & 0x80)) have_cfg = 1;

    if (!have_cfg) {
        controller_state = -1;
        cycle = 0;
        log_printf("[PS2 ] no i8042 command byte, keeping firmware PS/2 mapping\n");
        return;
    }

    /* Wanted state: IRQ1 + IRQ12 enabled, both device clocks running and
     * scancode translation on (this kernel decodes PS/2 Set 1). Bits 4 and 5
     * disable the clocks and are the classic cause of "both devices dead". */
    unsigned char want = (unsigned char)((cfg | 0x03 | 0x40) & ~0x30);
    want &= 0x7F;

    if (want == cfg) {
        /* Firmware already left the controller in a usable state: do not write
         * anything at all, so the SMM legacy path stays intact. */
        log_printf("[PS2 ] firmware PS/2 state ok (cfg=0x%X), not rewriting\n", cfg);
    } else {
        kbc_cmd(0x60);
        kbc_write(want);
        kbc_settle();

        unsigned char check = 0;
        if (!(kbc_cmd(0x20) && kbc_read(&check) && check == want)) {
            controller_state = -1;
            cycle = 0;
            log_printf("[PS2 ] command byte not writable, using firmware mapping\n");
            return;
        }
        log_printf("[PS2 ] command byte 0x%X -> 0x%X\n", cfg, want);
    }

    /* Enable the auxiliary interface only when firmware gated its clock. */
    if (cfg & 0x20) {
        kbc_cmd(0xA8);
        kbc_settle();
    }

    /* Ask the mouse for defaults and streaming, but only while it really
     * acknowledges. A silent port means the firmware is emulating it and must
     * be left running exactly as the BIOS configured it. */
    if (mouse_cmd(0xF6)) {
        kbc_settle();
        if (!mouse_cmd(0xF4)) log_printf("[PS2 ] mouse rejected streaming enable\n");
    } else {
        log_printf("[PS2 ] no PS/2 mouse ACK, keeping firmware streaming state\n");
    }

    controller_state = 1;
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

/* USB HID mouse reports must not share the PS/2 3-byte packet state in
 * cycle/buf: a PS/2 packet can be partially buffered when a USB report
 * arrives, and mixing the bytes corrupts both devices. Keep the states
 * separate and update the shared cursor/button state atomically. */
void mouse_handle_usb_report(unsigned char buttons, int dx, int dy)
{
    unsigned int flags = irq_save_flags();
    btn = buttons & 0x07;
    mx += dx * mouse_speed / 2;
    my += dy * mouse_speed / 2;   /* HID Y positive = down */
    if (mx < 0) mx = 0;
    if (mx > screen_w - 1) mx = screen_w - 1;
    if (my < 0) my = 0;
    if (my > screen_h - 1) my = screen_h - 1;
    irq_restore_flags(flags);
}

void mouse_get(int* x, int* y, int* b)
{
    unsigned int flags = irq_save_flags();
    if (x) *x = mx;
    if (y) *y = my;
    if (b) *b = btn;
    irq_restore_flags(flags);
}

void mouse_draw(unsigned char* vga)
{
    if (!init_done) return;
    int bpp_bytes = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
    unsigned int flags = irq_save_flags();
    int x = mx, y = my;
    irq_restore_flags(flags);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) {
            if (y+i < screen_h && x+j < screen_w) {
                int offset = (y+i) * screen_pitch + (x+j) * bpp_bytes;
                if (screen_bpp <= 8) saved[i][j] = vga[offset];
                else if (screen_bpp <= 16) saved[i][j] = *((unsigned short*)&vga[offset]);
                else if (screen_bpp == 24) {
                    saved[i][j] = vga[offset] | (vga[offset+1] << 8) | (vga[offset+2] << 16);
                } else {
                    saved[i][j] = *((unsigned int*)&vga[offset]);
                }
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
        int bpp_bytes = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                if (saved_y+i < screen_h && saved_x+j < screen_w) {
                    int offset = (saved_y+i) * screen_pitch + (saved_x+j) * bpp_bytes;
                    if (screen_bpp <= 8) vga[offset] = (unsigned char)saved[i][j];
                    else if (screen_bpp <= 16) *((unsigned short*)&vga[offset]) = (unsigned short)saved[i][j];
                    else if (screen_bpp == 24) {
                        vga[offset]   = (unsigned char)(saved[i][j] & 0xFF);
                        vga[offset+1] = (unsigned char)((saved[i][j] >> 8) & 0xFF);
                        vga[offset+2] = (unsigned char)((saved[i][j] >> 16) & 0xFF);
                    } else {
                        *((unsigned int*)&vga[offset]) = saved[i][j];
                    }
                }
    }
    saved_x = -1;
    saved_y = -1;
}

void mouse_invalidate(void)
{
    saved_x = -1;
    saved_y = -1;
}