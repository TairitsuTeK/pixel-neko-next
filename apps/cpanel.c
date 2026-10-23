#include "cpanel.h"
#include "font.h"
#include "mouse.h"
#include "wm.h"
#include "port.h"
#include "gfx_accel.h"

static int slider_val = 2;
int revert_timer = 0;
static int prev_w = 800, prev_h = 600, prev_bpp = 16;
extern void set_resolution(int w, int h, int bpp);
extern volatile int global_tick;
extern int redraw;

int g_desktop_pattern = 0; // 0..5

static int sel_res_idx = 2; // Default 800x600
static int sel_bpp_idx = 1; // Default 16-bit
static int res_dropdown_open = 0;
static int bpp_dropdown_open = 0;

static const int res_w[4] = { 320, 640, 800, 1024 };
static const int res_h[4] = { 200, 480, 600, 768 };
static const char* res_names[4] = { "320 x 200", "640 x 480", "800 x 600", "1024 x 768" };

static const int bpp_vals[4] = { 8, 16, 24, 32 };
static const char* bpp_names[4] = { "8-bit  (256 Col)", "16-bit (HighCol)", "24-bit (TrueCol)", "32-bit (TrueCol)" };

static const char* pat_names[6] = {
    "0: Stipple 50%", "1: Diagonal", "2: Micro Dots",
    "3: Brick Tile", "4: Slate Gray", "5: Solid Gray"
};

static unsigned char cmos_get_sec(void) {
    int timeout = 10000;
    outb(0x70, 0x0A);
    while ((inb(0x71) & 0x80) && --timeout);
    outb(0x70, 0x00);
    return inb(0x71);
}

void check_revert_timer(void)
{
    if (revert_timer <= 0) return;
    static unsigned char last_cmos_sec = 0xFF;
    unsigned char cur_sec = cmos_get_sec();
    if (last_cmos_sec == 0xFF) last_cmos_sec = cur_sec;

    if (cur_sec != last_cmos_sec) {
        last_cmos_sec = cur_sec;
        if (revert_timer > 0) {
            revert_timer--;
            redraw = 1;
            if (revert_timer == 0) {
                set_resolution(prev_w, prev_h, prev_bpp);
                gfx_accel_set_mode(prev_w, prev_h, prev_bpp);
                redraw = 1;
            }
        }
    }
}

void cpanel_init(void)
{
    slider_val = mouse_get_speed();

    // Match current resolution & bpp
    for (int i = 0; i < 4; i++) {
        if (screen_w == res_w[i] && screen_h == res_h[i]) sel_res_idx = i;
    }
    for (int i = 0; i < 4; i++) {
        if (screen_bpp == bpp_vals[i]) sel_bpp_idx = i;
    }
}

static void draw_btn(unsigned char* v, int bx, int by, int bw, int bh, const char* text)
{
    for(int y=by; y<by+bh; y++) for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, y, 7);
    for(int x=bx; x<bx+bw; x++) { PUT_PIXEL(v, x, by, 15); PUT_PIXEL(v, x, by+bh-1, 0); }
    for(int y=by; y<by+bh; y++) { PUT_PIXEL(v, bx, y, 15); PUT_PIXEL(v, bx+bw-1, y, 0); }
    font_draw_text(v, bx+4, by+(bh/2)-4, text, 0);
}

static void draw_combo_box(unsigned char* v, int bx, int by, int bw, int bh, const char* text)
{
    for(int y=by; y<by+bh; y++) for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, y, 15);
    for(int x=bx; x<bx+bw; x++) { PUT_PIXEL(v, x, by, 0); PUT_PIXEL(v, x, by+bh-1, 8); }
    for(int y=by; y<by+bh; y++) { PUT_PIXEL(v, bx, y, 0); PUT_PIXEL(v, bx+bw-1, y, 8); }

    // Down arrow button on right
    int abx = bx + bw - 16;
    for(int y=by+1; y<by+bh-1; y++) for(int x=abx; x<bx+bw-1; x++) PUT_PIXEL(v, x, y, 7);
    for(int y=by+1; y<by+bh-1; y++) PUT_PIXEL(v, abx, y, 8);
    font_draw_text(v, abx+4, by+2, "v", 0);

    font_draw_text(v, bx+4, by+3, text, 0);
}

void cpanel_draw(unsigned char* v, int wx, int wy, int ww, int wh)
{
    (void)ww; (void)wh;

    // 1. Mouse Speed Slider
    font_draw_text(v, wx+8, wy+20, "Mouse Speed:", 0);
    int tx = wx+95, ty = wy+22;
    for(int y=ty; y<ty+6; y++) for(int x=tx; x<tx+100; x++) PUT_PIXEL(v, x, y, 7);
    for(int x=tx; x<tx+100; x++) { PUT_PIXEL(v, x, ty, 0); PUT_PIXEL(v, x, ty+5, 0); }
    for(int y=ty; y<ty+6; y++) { PUT_PIXEL(v, tx, y, 0); PUT_PIXEL(v, tx+99, y, 0); }
    for(int i=0; i<5; i++) {
        int sx = tx + i*20 + 8;
        for(int y=ty-2; y<ty+8; y++) for(int x=sx; x<sx+2; x++) PUT_PIXEL(v, x, y, 0);
    }
    int hx = tx + slider_val*20 + 4, hy = ty - 4;
    for(int y=hy; y<hy+14; y++) for(int x=hx; x<hx+10; x++) PUT_PIXEL(v, x, y, 9);
    for(int x=hx; x<hx+10; x++) { PUT_PIXEL(v, x, hy, 11); PUT_PIXEL(v, x, hy+13, 1); }
    for(int y=hy; y<hy+14; y++) { PUT_PIXEL(v, hx, y, 11); PUT_PIXEL(v, hx+9, y, 1); }

    // 2. Display Settings Labels
    font_draw_text(v, wx+8, wy+44, "Resolution:", 0);
    draw_combo_box(v, wx+95, wy+42, 145, 16, res_names[sel_res_idx]);

    font_draw_text(v, wx+8, wy+66, "Color Depth:", 0);
    draw_combo_box(v, wx+95, wy+64, 145, 16, bpp_names[sel_bpp_idx]);

    // 3. Desktop Pattern
    font_draw_text(v, wx+8, wy+88, "Pattern:", 0);
    draw_btn(v, wx+95, wy+86, 20, 16, "<");
    draw_btn(v, wx+220, wy+86, 20, 16, ">");
    font_draw_text(v, wx+120, wy+89, pat_names[g_desktop_pattern], 0);

    // 4. Driver Status text
    const char* drv_name = gfx_accel_get_name();
    font_draw_text(v, wx+8, wy+110, "Driver:", 0);
    font_draw_text(v, wx+60, wy+110, drv_name, gfx_accel_is_active() ? 10 : 8);

    // 5. Apply / OK Buttons
    if (revert_timer > 0) {
        int sec = revert_timer;
        char buf[30] = "Revert in   s...";
        buf[10] = '0' + (sec / 10);
        buf[11] = '0' + (sec % 10);
        font_draw_text(v, wx+10, wy+135, buf, 4);
        draw_btn(v, wx+150, wy+132, 90, 20, "Keep (OK)");
    } else {
        draw_btn(v, wx+35, wy+132, 90, 20, "Apply Mode");
        draw_btn(v, wx+145, wy+132, 90, 20, "OK");
    }

    // 6. Draw Dropdown Menus if Open (Rendered on top)
    if (res_dropdown_open) {
        int dx = wx+95, dy = wy+58, dw = 145, dh = 4 * 16 + 2;
        for(int y=dy; y<dy+dh; y++) for(int x=dx; x<dx+dw; x++) PUT_PIXEL(v, x, y, 15);
        for(int x=dx; x<dx+dw; x++) { PUT_PIXEL(v, x, dy, 0); PUT_PIXEL(v, x, dy+dh-1, 0); }
        for(int y=dy; y<dy+dh; y++) { PUT_PIXEL(v, dx, y, 0); PUT_PIXEL(v, dx+dw-1, y, 0); }
        for (int i = 0; i < 4; i++) {
            int iy = dy + 1 + i * 16;
            if (i == sel_res_idx) {
                for(int y=iy; y<iy+16; y++) for(int x=dx+1; x<dx+dw-1; x++) PUT_PIXEL(v, x, y, 9);
            }
            font_draw_text(v, dx+4, iy+2, res_names[i], i==sel_res_idx ? 15 : 0);
        }
    } else if (bpp_dropdown_open) {
        int dx = wx+95, dy = wy+80, dw = 145, dh = 4 * 16 + 2;
        for(int y=dy; y<dy+dh; y++) for(int x=dx; x<dx+dw; x++) PUT_PIXEL(v, x, y, 15);
        for(int x=dx; x<dx+dw; x++) { PUT_PIXEL(v, x, dy, 0); PUT_PIXEL(v, x, dy+dh-1, 0); }
        for(int y=dy; y<dy+dh; y++) { PUT_PIXEL(v, dx, y, 0); PUT_PIXEL(v, dx+dw-1, y, 0); }
        for (int i = 0; i < 4; i++) {
            int iy = dy + 1 + i * 16;
            int valid = gfx_accel_supports_bpp(res_w[sel_res_idx], res_h[sel_res_idx], bpp_vals[i]);
            if (i == sel_bpp_idx && valid) {
                for(int y=iy; y<iy+16; y++) for(int x=dx+1; x<dx+dw-1; x++) PUT_PIXEL(v, x, y, 9);
            }
            int fg = !valid ? 8 : (i==sel_bpp_idx ? 15 : 0);
            font_draw_text(v, dx+4, iy+2, bpp_names[i], fg);
            if (!valid) {
                font_draw_text(v, dx+110, iy+2, "N/A", 8);
            }
        }
    }
}

void cpanel_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh)
{
    (void)v; (void)ww; (void)wh;

    // Handle open Dropdown clicks first
    if (res_dropdown_open) {
        int dx = wx+95, dy = wy+58, dw = 145;
        if (mx >= dx && mx < dx+dw && my >= dy && my < dy + 4*16) {
            int idx = (my - dy) / 16;
            if (idx >= 0 && idx < 4) {
                sel_res_idx = idx;
                // Auto-downgrade bpp if new resolution exceeds VRAM
                if (!gfx_accel_supports_bpp(res_w[sel_res_idx], res_h[sel_res_idx], bpp_vals[sel_bpp_idx])) {
                    sel_bpp_idx = 1; // Fallback to 16-bit
                    if (!gfx_accel_supports_bpp(res_w[sel_res_idx], res_h[sel_res_idx], bpp_vals[sel_bpp_idx])) {
                        sel_bpp_idx = 0; // Fallback to 8-bit
                    }
                }
            }
        }
        res_dropdown_open = 0;
        redraw = 1;
        return;
    }

    if (bpp_dropdown_open) {
        int dx = wx+95, dy = wy+80, dw = 145;
        if (mx >= dx && mx < dx+dw && my >= dy && my < dy + 4*16) {
            int idx = (my - dy) / 16;
            if (idx >= 0 && idx < 4) {
                if (gfx_accel_supports_bpp(res_w[sel_res_idx], res_h[sel_res_idx], bpp_vals[idx])) {
                    sel_bpp_idx = idx;
                }
            }
        }
        bpp_dropdown_open = 0;
        redraw = 1;
        return;
    }

    // Mouse speed slider
    int tx = wx+95, ty = wy+22;
    if (mx >= tx && mx < tx+100 && my >= ty-4 && my < ty+16) {
        int val = (mx - tx) / 20;
        if (val < 0) val = 0;
        if (val > 4) val = 4;
        slider_val = val;
        mouse_set_speed(val);
        redraw = 1;
        return;
    }

    // Open Resolution Dropdown
    if (mx >= wx+95 && mx < wx+240 && my >= wy+42 && my < wy+58) {
        res_dropdown_open = 1;
        bpp_dropdown_open = 0;
        redraw = 1;
        return;
    }

    // Open Color Depth Dropdown
    if (mx >= wx+95 && mx < wx+240 && my >= wy+64 && my < wy+80) {
        bpp_dropdown_open = 1;
        res_dropdown_open = 0;
        redraw = 1;
        return;
    }

    // Pattern Previous '<'
    if (mx >= wx+95 && mx < wx+115 && my >= wy+86 && my < wy+102) {
        g_desktop_pattern = (g_desktop_pattern + 5) % 6;
        redraw = 1;
        return;
    }

    // Pattern Next '>'
    if (mx >= wx+220 && mx < wx+240 && my >= wy+86 && my < wy+102) {
        g_desktop_pattern = (g_desktop_pattern + 1) % 6;
        redraw = 1;
        return;
    }

    // Keep / Revert Timer Cancel
    if (revert_timer > 0) {
        if (mx >= wx+150 && mx < wx+240 && my >= wy+132 && my < wy+152) {
            revert_timer = 0;
            redraw = 1;
        }
        return;
    }

    // Apply Button
    if (mx >= wx+35 && mx < wx+125 && my >= wy+132 && my < wy+152) {
        int tw = res_w[sel_res_idx];
        int th = res_h[sel_res_idx];
        int tb = bpp_vals[sel_bpp_idx];

        if (gfx_accel_supports_bpp(tw, th, tb)) {
            prev_w = screen_w; prev_h = screen_h; prev_bpp = screen_bpp;
            set_resolution(tw, th, tb);
            gfx_accel_set_mode(tw, th, tb);
            revert_timer = 10;
            redraw = 1;
        }
        return;
    }

    // OK Button
    if (mx >= wx+145 && mx < wx+235 && my >= wy+132 && my < wy+152) {
        int tw = res_w[sel_res_idx];
        int th = res_h[sel_res_idx];
        int tb = bpp_vals[sel_bpp_idx];

        if (tw != screen_w || th != screen_h || tb != screen_bpp) {
            if (gfx_accel_supports_bpp(tw, th, tb)) {
                prev_w = screen_w; prev_h = screen_h; prev_bpp = screen_bpp;
                set_resolution(tw, th, tb);
                gfx_accel_set_mode(tw, th, tb);
            }
        }
        revert_timer = 0;
        redraw = 1;
        return;
    }
}