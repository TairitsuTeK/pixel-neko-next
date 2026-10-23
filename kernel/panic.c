#include "panic.h"
#include "font.h"
#include "wm.h"
#include "port.h"
#include "power.h"
#include "cpu.h"

extern int screen_w, screen_h;
extern unsigned char* lfb;

static void hex32_to_str(unsigned int val, char* out) {
    const char hex[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) {
        out[2 + i] = hex[(val >> ((7 - i) * 4)) & 0xF];
    }
    out[10] = '\0';
}

// 24x24 Classic Mac OS Burning Bomb Pixel Art
static const char* bomb_art[24] = {
    "            * *         ",
    "           *   *        ",
    "            ***         ",
    "             |          ",
    "           --+--        ",
    "          |     |       ",
    "        XX.......XX     ",
    "       X...........X    ",
    "      X..XX.........X   ",
    "     X..X  X.........X  ",
    "    X...X  X..........X ",
    "    X....XX...........X ",
    "    X.................X ",
    "    X.................X ",
    "    X.................X ",
    "     X...............X  ",
    "      X.............X   ",
    "       X...........X    ",
    "        XX.......XX     ",
    "          XXXXXXX       ",
    "                        ",
    "                        ",
    "                        ",
    "                        "
};

void panic_show_exception(struct trap_frame* frame, const char* name) {
    unsigned char* v = backbuffer;

    int box_w = 400;
    int box_h = 160;
    if (box_w > screen_w - 20) box_w = screen_w - 20;
    if (box_h > screen_h - 20) box_h = screen_h - 20;

    int bx = (screen_w - box_w) / 2;
    int by = (screen_h - box_h) / 2;

    // Drop Shadow
    for (int y = by + 4; y < by + box_h + 4 && y < screen_h; y++) {
        for (int x = bx + 4; x < bx + box_w + 4 && x < screen_w; x++) {
            PUT_PIXEL(v, x, y, 8);
        }
    }

    // Box body (Platinum gray)
    for (int y = by; y < by + box_h && y < screen_h; y++) {
        for (int x = bx; x < bx + box_w && x < screen_w; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }

    // Classic Mac Double Outline
    for (int x = bx; x < bx + box_w; x++) {
        PUT_PIXEL(v, x, by, 0);
        PUT_PIXEL(v, x, by + box_h - 1, 0);
        PUT_PIXEL(v, x, by + 2, 0);
        PUT_PIXEL(v, x, by + box_h - 3, 0);
    }
    for (int y = by; y < by + box_h; y++) {
        PUT_PIXEL(v, bx, y, 0);
        PUT_PIXEL(v, bx + box_w - 1, y, 0);
        PUT_PIXEL(v, bx + 2, y, 0);
        PUT_PIXEL(v, bx + box_w - 3, y, 0);
    }

    // Draw Bomb Icon
    int ix = bx + 16, iy = by + 16;
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 24; c++) {
            char ch = bomb_art[r][c];
            if (ch == '*') PUT_PIXEL(v, ix + c, iy + r, 12); // Orange spark
            else if (ch == '|') PUT_PIXEL(v, ix + c, iy + r, 6); // Brown fuse
            else if (ch == '+') PUT_PIXEL(v, ix + c, iy + r, 8); // Fuse cap
            else if (ch == 'X') PUT_PIXEL(v, ix + c, iy + r, 0); // Bomb outline
            else if (ch == '.') PUT_PIXEL(v, ix + c, iy + r, 0); // Bomb black fill
        }
    }
    // Bomb shine reflection
    PUT_PIXEL(v, ix + 8, iy + 9, 15);
    PUT_PIXEL(v, ix + 9, iy + 9, 15);
    PUT_PIXEL(v, ix + 8, iy + 10, 15);

    // Text Information
    font_draw_text(v, bx + 56, by + 16, "Sorry, a system error occurred.", 0);
    font_draw_text(v, bx + 56, by + 34, name ? name : "Fatal Exception", 4);

    char reg_buf[64] = "EIP: ";
    char hbuf[16];
    hex32_to_str(frame ? frame->eip : 0, hbuf);
    int p = 5;
    for (int i = 0; hbuf[i]; i++) reg_buf[p++] = hbuf[i];
    reg_buf[p++] = ' '; reg_buf[p++] = 'C'; reg_buf[p++] = 'S'; reg_buf[p++] = ':'; reg_buf[p++] = ' ';
    hex32_to_str(frame ? frame->cs : 0, hbuf);
    for (int i = 0; hbuf[i]; i++) reg_buf[p++] = hbuf[i];
    reg_buf[p] = '\0';
    font_draw_text(v, bx + 56, by + 54, reg_buf, 0);

    // EFLAGS & CR2
    unsigned int cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    char flag_buf[64] = "EFLAGS: ";
    hex32_to_str(frame ? frame->eflags : 0, hbuf);
    p = 8;
    for (int i = 0; hbuf[i]; i++) flag_buf[p++] = hbuf[i];
    flag_buf[p++] = ' '; flag_buf[p++] = 'C'; flag_buf[p++] = 'R'; flag_buf[p++] = '2'; flag_buf[p++] = ':'; flag_buf[p++] = ' ';
    hex32_to_str(cr2, hbuf);
    for (int i = 0; hbuf[i]; i++) flag_buf[p++] = hbuf[i];
    flag_buf[p] = '\0';
    font_draw_text(v, bx + 56, by + 72, flag_buf, 8);

    font_draw_text(v, bx + 56, by + 90, cpu_model_string, 8);

    // [ Restart Computer ] Button
    int btn_w = 150, btn_h = 24;
    int btn_x = bx + (box_w - btn_w) / 2;
    int btn_y = by + box_h - 34;

    for (int y = btn_y; y < btn_y + btn_h; y++) {
        for (int x = btn_x; x < btn_x + btn_w; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }
    for (int x = btn_x; x < btn_x + btn_w; x++) {
        PUT_PIXEL(v, x, btn_y, 15);
        PUT_PIXEL(v, x, btn_y + btn_h - 1, 0);
    }
    for (int y = btn_y; y < btn_y + btn_h; y++) {
        PUT_PIXEL(v, btn_x, y, 15);
        PUT_PIXEL(v, btn_x + btn_w - 1, y, 0);
    }
    font_draw_text(v, btn_x + 12, btn_y + 6, "Restart Computer", 0);

    copy_fullscreen_to_lfb(v);

    // Poll for Keyboard (any key) or Mouse reboot
    while (1) {
        if (inb(0x64) & 0x01) {
            unsigned char st = inb(0x64);
            unsigned char data = inb(0x60);
            (void)data;
            if (!(st & 0x20)) {
                // Key pressed -> safe reboot
                sys_reboot();
            }
        }
        asm volatile("rep; nop");
    }
}

void panic_halt(const char* message) {
    panic_show_exception(0, message);
}
