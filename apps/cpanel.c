#include "cpanel.h"
#include "font.h"
#include "mouse.h"
#include "wm.h"
#include "port.h"
#include "gfx_accel.h"
#include "wallpaper.h"
#include "settings.h"

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
    int tw = 0; while (text[tw]) tw++;
    int tx = bx + (bw - tw * 8) / 2;
    font_draw_text(v, tx, by+(bh/2)-4, text, 0);
}

static void draw_btn_sel(unsigned char* v, int bx, int by, int bw, int bh, const char* text, int is_sel)
{
    int bg = is_sel ? 9 : 7;
    int fg = is_sel ? 15 : 0;
    for(int y=by; y<by+bh; y++) for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, y, bg);
    for(int x=bx; x<bx+bw; x++) { PUT_PIXEL(v, x, by, is_sel?0:15); PUT_PIXEL(v, x, by+bh-1, is_sel?15:0); }
    for(int y=by; y<by+bh; y++) { PUT_PIXEL(v, bx, y, is_sel?0:15); PUT_PIXEL(v, bx+bw-1, y, is_sel?15:0); }
    int tw = 0; while (text[tw]) tw++;
    int tx = bx + (bw - tw * 8) / 2;
    font_draw_text(v, tx, by+(bh/2)-4, text, fg);
}

static void draw_combo_box(unsigned char* v, int bx, int by, int bw, int bh, const char* text)
{
    for(int y=by; y<by+bh; y++) for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, y, 15);
    for(int x=bx; x<bx+bw; x++) { PUT_PIXEL(v, x, by, 0); PUT_PIXEL(v, x, by+bh-1, 8); }
    for(int y=by; y<by+bh; y++) { PUT_PIXEL(v, bx, y, 0); PUT_PIXEL(v, bx+bw-1, y, 8); }

    // Down arrow button on right
    int abx = bx + bw - 18;
    for(int y=by+1; y<by+bh-1; y++) for(int x=abx; x<bx+bw-1; x++) PUT_PIXEL(v, x, y, 7);
    for(int y=by+1; y<by+bh-1; y++) PUT_PIXEL(v, abx, y, 8);
    font_draw_text(v, abx+5, by+3, "v", 0);

    font_draw_text(v, bx+6, by+3, text, 0);
}

void cpanel_draw(unsigned char* v, int wx, int wy, int ww, int wh)
{
    (void)ww; (void)wh;

    // 1. Mouse Speed Slider
    font_draw_text(v, wx+10, wy+26, "Mouse Speed:", 0);
    int tx = wx+120, ty = wy+28;
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
    font_draw_text(v, wx+10, wy+52, "Resolution:", 0);
    draw_combo_box(v, wx+120, wy+50, 160, 18, res_names[sel_res_idx]);

    font_draw_text(v, wx+10, wy+78, "Color Depth:", 0);
    draw_combo_box(v, wx+120, wy+76, 160, 18, bpp_names[sel_bpp_idx]);

    // 3. Desktop Pattern
    font_draw_text(v, wx+10, wy+104, "Pattern:", 0);
    draw_btn(v, wx+120, wy+102, 20, 18, "<");
    draw_btn(v, wx+260, wy+102, 20, 18, ">");
    font_draw_text(v, wx+144, wy+104, pat_names[g_desktop_pattern], 0);

    // 4. Wallpaper Section
    font_draw_text(v, wx+10, wy+128, "Wallpaper:", 0);
    if (wallpaper_is_active()) {
        wallpaper_mode_t cur_m = wallpaper_get_mode();
        draw_btn_sel(v, wx+95,  wy+126, 36, 18, "Fill", cur_m == WP_MODE_FILL);
        draw_btn_sel(v, wx+135, wy+126, 32, 18, "Fit",  cur_m == WP_MODE_FIT);
        draw_btn_sel(v, wx+171, wy+126, 46, 18, "Stret", cur_m == WP_MODE_STRETCH);
        draw_btn_sel(v, wx+221, wy+126, 36, 18, "Tile", cur_m == WP_MODE_TILE);
        draw_btn(v, wx+261, wy+126, 32, 18, "Off");
    } else {
        font_draw_text(v, wx+95, wy+128, "None (Default Pattern)", 8);
    }

    // 5. Driver Status text
    const char* drv_name = gfx_accel_get_name();
    font_draw_text(v, wx+10, wy+152, "Driver:", 0);
    font_draw_text(v, wx+76, wy+152, drv_name, gfx_accel_is_active() ? 10 : 8);

    // 6. Apply / OK Buttons
    if (revert_timer > 0) {
        int sec = revert_timer;
        char buf[30] = "Revert in   s...";
        buf[10] = '0' + (sec / 10);
        buf[11] = '0' + (sec % 10);
        font_draw_text(v, wx+16, wy+184, buf, 4);
        draw_btn(v, wx+165, wy+180, 105, 22, "Keep (OK)");
    } else {
        draw_btn(v, wx+40, wy+180, 105, 22, "Apply Mode");
        draw_btn(v, wx+165, wy+180, 105, 22, "OK");
    }

    // 7. Draw Dropdown Menus if Open (Rendered on top)
    if (res_dropdown_open) {
        int dx = wx+120, dy = wy+68, dw = 160, dh = 4 * 18 + 2;
        for(int y=dy; y<dy+dh; y++) for(int x=dx; x<dx+dw; x++) PUT_PIXEL(v, x, y, 15);
        for(int x=dx; x<dx+dw; x++) { PUT_PIXEL(v, x, dy, 0); PUT_PIXEL(v, x, dy+dh-1, 0); }
        for(int y=dy; y<dy+dh; y++) { PUT_PIXEL(v, dx, y, 0); PUT_PIXEL(v, dx+dw-1, y, 0); }
        for (int i = 0; i < 4; i++) {
            int iy = dy + 1 + i * 18;
            if (i == sel_res_idx) {
                for(int y=iy; y<iy+18; y++) for(int x=dx+1; x<dx+dw-1; x++) PUT_PIXEL(v, x, y, 9);
            }
            font_draw_text(v, dx+6, iy+3, res_names[i], i==sel_res_idx ? 15 : 0);
        }
    } else if (bpp_dropdown_open) {
        int dx = wx+120, dy = wy+94, dw = 160, dh = 4 * 18 + 2;
        for(int y=dy; y<dy+dh; y++) for(int x=dx; x<dx+dw; x++) PUT_PIXEL(v, x, y, 15);
        for(int x=dx; x<dx+dw; x++) { PUT_PIXEL(v, x, dy, 0); PUT_PIXEL(v, x, dy+dh-1, 0); }
        for(int y=dy; y<dy+dh; y++) { PUT_PIXEL(v, dx, y, 0); PUT_PIXEL(v, dx+dw-1, y, 0); }
        for (int i = 0; i < 4; i++) {
            int iy = dy + 1 + i * 18;
            int valid = gfx_accel_supports_bpp(res_w[sel_res_idx], res_h[sel_res_idx], bpp_vals[i]);
            if (i == sel_bpp_idx && valid) {
                for(int y=iy; y<iy+18; y++) for(int x=dx+1; x<dx+dw-1; x++) PUT_PIXEL(v, x, y, 9);
            }
            int fg = !valid ? 8 : (i==sel_bpp_idx ? 15 : 0);
            const char* item_txt = bpp_names[i];
            if (!valid) {
                unsigned int bytes_per_pixel = (bpp_vals[i] <= 8) ? 1 : ((bpp_vals[i] <= 16) ? 2 : ((bpp_vals[i] <= 24) ? 3 : 4));
                unsigned int vram_needed = (unsigned int)res_w[sel_res_idx] * (unsigned int)res_h[sel_res_idx] * bytes_per_pixel;
                unsigned int max_vram = gfx_accel_get_vram_size();
                int is_no_vram = (max_vram > 0 && vram_needed > max_vram);
                if (bpp_vals[i] == 24) item_txt = is_no_vram ? "24-bit (No VRAM)" : "24-bit (Not Supported)";
                else if (bpp_vals[i] == 32) item_txt = is_no_vram ? "32-bit (No VRAM)" : "32-bit (Not Supported)";
            }
            font_draw_text(v, dx+6, iy+3, item_txt, fg);
        }
    }
}

void cpanel_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh)
{
    (void)v; (void)ww; (void)wh;

    // Handle open Dropdown clicks first
    if (res_dropdown_open) {
        int dx = wx+120, dy = wy+68, dw = 160;
        if (mx >= dx && mx < dx+dw && my >= dy && my < dy + 4*18) {
            int idx = (my - dy) / 18;
            if (idx >= 0 && idx < 4) {
                sel_res_idx = idx;
                // If current selected bpp is not valid for new resolution, fall back
                if (!gfx_accel_supports_bpp(res_w[sel_res_idx], res_h[sel_res_idx], bpp_vals[sel_bpp_idx])) {
                    sel_bpp_idx = 1; // Try 16-bit
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
        int dx = wx+120, dy = wy+94, dw = 160;
        if (mx >= dx && mx < dx+dw && my >= dy && my < dy + 4*18) {
            int idx = (my - dy) / 18;
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
    int tx = wx+120, ty = wy+28;
    if (mx >= tx && mx < tx+100 && my >= ty-4 && my < ty+18) {
        int val = (mx - tx) / 20;
        if (val < 0) val = 0;
        if (val > 4) val = 4;
        slider_val = val;
        mouse_set_speed(val);
        redraw = 1;
        return;
    }

    // Open Resolution Dropdown
    if (mx >= wx+120 && mx < wx+280 && my >= wy+50 && my < wy+68) {
        res_dropdown_open = 1;
        bpp_dropdown_open = 0;
        redraw = 1;
        return;
    }

    // Open Color Depth Dropdown
    if (mx >= wx+120 && mx < wx+280 && my >= wy+76 && my < wy+94) {
        bpp_dropdown_open = 1;
        res_dropdown_open = 0;
        redraw = 1;
        return;
    }

    // Pattern Previous '<'
    if (mx >= wx+120 && mx < wx+140 && my >= wy+102 && my < wy+120) {
        g_desktop_pattern = (g_desktop_pattern + 5) % 6;
        settings_set_pattern(g_desktop_pattern);
        settings_save();
        redraw = 1;
        return;
    }

    // Pattern Next '>'
    if (mx >= wx+260 && mx < wx+280 && my >= wy+102 && my < wy+120) {
        g_desktop_pattern = (g_desktop_pattern + 1) % 6;
        settings_set_pattern(g_desktop_pattern);
        settings_save();
        redraw = 1;
        return;
    }

    // Wallpaper mode buttons
    if (wallpaper_is_active()) {
        if (my >= wy+126 && my < wy+144) {
            if (mx >= wx+95 && mx < wx+131) {
                wallpaper_set_mode(WP_MODE_FILL);
                settings_save();
                redraw = 1;
                return;
            } else if (mx >= wx+135 && mx < wx+167) {
                wallpaper_set_mode(WP_MODE_FIT);
                settings_save();
                redraw = 1;
                return;
            } else if (mx >= wx+171 && mx < wx+217) {
                wallpaper_set_mode(WP_MODE_STRETCH);
                settings_save();
                redraw = 1;
                return;
            } else if (mx >= wx+221 && mx < wx+257) {
                wallpaper_set_mode(WP_MODE_TILE);
                settings_save();
                redraw = 1;
                return;
            } else if (mx >= wx+261 && mx < wx+293) {
                wallpaper_clear();
                settings_clear_wallpaper();
                settings_save();
                redraw = 1;
                return;
            }
        }
    }

    // Keep / Revert Timer Cancel
    if (revert_timer > 0) {
        if (mx >= wx+165 && mx < wx+270 && my >= wy+180 && my < wy+202) {
            revert_timer = 0;
            settings_set_resolution(screen_w, screen_h, screen_bpp);
            settings_save();
            redraw = 1;
        }
        return;
    }

    // Apply Button
    if (mx >= wx+40 && mx < wx+145 && my >= wy+180 && my < wy+202) {
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
    if (mx >= wx+165 && mx < wx+270 && my >= wy+180 && my < wy+202) {
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
        settings_set_resolution(screen_w, screen_h, screen_bpp);
        settings_set_pattern(g_desktop_pattern);
        settings_save();
        redraw = 1;
        return;
    }
}