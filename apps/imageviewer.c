#include "imageviewer.h"
#include "image_dec.h"
#include "wallpaper.h"
#include "font.h"
#include "wm.h"
#include "mm.h"
#include "vfs.h"
#include "speaker.h"

int imv = 0;
int imm = 0;

static image_t g_img;
static int g_img_loaded = 0;
static char g_img_path[256] = "";
static char g_title[64] = "Image Viewer";

// Zoom & Pan state
// zoom_mode: 0 = Fit to window, > 0 = Fixed percentage (e.g. 100 = 100%)
static int g_zoom_percent = 0; // 0 means Fit
static const int g_zoom_steps[] = { 12, 25, 33, 50, 75, 100, 150, 200, 300, 400 };
#define NUM_ZOOM_STEPS (int)(sizeof(g_zoom_steps) / sizeof(g_zoom_steps[0]))

static int g_pan_x = 0;
static int g_pan_y = 0;
static int g_is_panning = 0;
static int g_drag_last_x = 0;
static int g_drag_last_y = 0;

// Wallpaper popup menu
static int g_wp_menu_open = 0;
static int g_wp_menu_x = 0;
static int g_wp_menu_y = 0;

// OOM / Error alert modal
static int g_error_modal = 0;
static char g_err_title[32] = "";
static char g_err_msg[64] = "";
static char g_err_msg2[64] = "";

extern int redraw;

static void my_str_cat(char* d, const char* s) {
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = '\0';
}

static void my_int_to_str(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int p = 0;
    int is_neg = 0;
    if (n < 0) { is_neg = 1; n = -n; }
    while (n > 0) {
        tmp[p++] = '0' + (n % 10);
        n /= 10;
    }
    int d = 0;
    if (is_neg) buf[d++] = '-';
    while (p > 0) buf[d++] = tmp[--p];
    buf[d] = '\0';
}

// Generate an ultra-crisp 4K procedural test pattern in memory if no file loaded
static void generate_sample_pattern(void) {
    image_free(&g_img);
    int w = 1920, h = 1080; // Crisp full-HD sample (fits nicely in memory, tests all zoom/pan)
    unsigned int pages = ((unsigned int)w * h * 4 + 4095) / 4096;
    if (pages > pmm_get_free_pages()) {
        w = 800; h = 600;
        pages = ((unsigned int)w * h * 4 + 4095) / 4096;
    }
    unsigned int* pix = (unsigned int*)pmm_alloc_pages(pages);
    if (!pix) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // Gradient sunset / geometric 32-bit color pattern
            unsigned char r = (unsigned char)((x * 255) / w);
            unsigned char g = (unsigned char)((y * 255) / h);
            unsigned char b = (unsigned char)(((w - x) * 200) / w + ((h - y) * 55) / h);
            // Checkerboard overlay
            if (((x / 64) ^ (y / 64)) & 1) {
                r = (r >> 1) + 64;
                g = (g >> 1) + 64;
                b = (b >> 1) + 64;
            }
            pix[y * w + x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
        }
    }

    g_img.width = w;
    g_img.height = h;
    g_img.pixels = pix;
    g_img.page_count = pages;
    g_img_loaded = 1;
    g_img_path[0] = '\0';
    g_title[0] = '\0';
    my_str_cat(g_title, "Image Viewer - Sample Image");
}

void imageviewer_init(void) {
    g_zoom_percent = 0; // Fit
    g_pan_x = 0;
    g_pan_y = 0;
    g_is_panning = 0;
    g_wp_menu_open = 0;
    g_error_modal = 0;

    if (!g_img_loaded) {
        generate_sample_pattern();
    }
}

void imageviewer_open_file(const char* path) {
    imageviewer_init();
    image_free(&g_img);
    g_img_loaded = 0;

    int i = 0;
    while (path[i] && i < 255) { g_img_path[i] = path[i]; i++; }
    g_img_path[i] = '\0';

    char err[64] = "";
    int ret = image_load_from_file(path, &g_img, err, sizeof(err));

    if (ret != 0) {
        // Trigger Out Of Memory / Error alert
        g_error_modal = 1;
        g_err_title[0] = '\0';
        my_str_cat(g_err_title, "Memory / Load Error");
        g_err_msg[0] = '\0';
        my_str_cat(g_err_msg, err[0] ? err : "Failed to open image file.");
        g_err_msg2[0] = '\0';
        my_str_cat(g_err_msg2, "Closing application to protect system.");
        speaker_beep();
        redraw = 1;
        return;
    }

    g_img_loaded = 1;
    g_zoom_percent = 0; // Fit
    g_pan_x = 0;
    g_pan_y = 0;

    // Build title
    g_title[0] = '\0';
    my_str_cat(g_title, "Viewer - ");
    const char* fname = path;
    for (int k = 0; path[k]; k++) {
        if (path[k] == '\\' || path[k] == '/') fname = &path[k + 1];
    }
    my_str_cat(g_title, fname);
    redraw = 1;
}

const char* imageviewer_get_title(void) {
    return g_title;
}

static void draw_btn(unsigned char* v, int bx, int by, int bw, int bh, const char* text) {
    for(int y=by; y<by+bh; y++) for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, y, 7);
    for(int x=bx; x<bx+bw; x++) { PUT_PIXEL(v, x, by, 15); PUT_PIXEL(v, x, by+bh-1, 0); }
    for(int y=by; y<by+bh; y++) { PUT_PIXEL(v, bx, y, 15); PUT_PIXEL(v, bx+bw-1, y, 0); }
    int tw = 0; while (text[tw]) tw++;
    int tx = bx + (bw - tw * 8) / 2;
    font_draw_text(v, tx, by + (bh / 2) - 4, text, 0);
}

void imageviewer_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    // 1. Toolbar area (y: wy + 19 to wy + 42)
    int tb_y = wy + 19;
    int tb_h = 24;
    for (int y = tb_y; y < tb_y + tb_h; y++) {
        for (int x = wx + 1; x < wx + ww - 1; x++) PUT_PIXEL(v, x, y, 7);
    }
    for (int x = wx; x < wx + ww; x++) PUT_PIXEL(v, x, tb_y + tb_h - 1, 8);

    // Toolbar buttons: [ - ] [ + ] [ 1:1 ] [ Fit ] [ Set Wallpaper v ]
    draw_btn(v, wx + 8, tb_y + 3, 26, 18, "-");
    draw_btn(v, wx + 38, tb_y + 3, 26, 18, "+");
    draw_btn(v, wx + 68, tb_y + 3, 36, 18, "1:1");
    draw_btn(v, wx + 108, tb_y + 3, 36, 18, "Fit");
    draw_btn(v, wx + 152, tb_y + 3, 110, 18, "Set Wallpaper");

    // 2. Status bar area (bottom 18px)
    int sb_h = 18;
    int sb_y = wy + wh - sb_h;
    for (int y = sb_y; y < wy + wh - 1; y++) {
        for (int x = wx + 1; x < wx + ww - 1; x++) PUT_PIXEL(v, x, y, 7);
    }
    for (int x = wx; x < wx + ww; x++) PUT_PIXEL(v, x, sb_y, 8);

    char stat_str[128] = "";
    if (g_img_loaded && g_img.width > 0) {
        char nbuf[16];
        my_int_to_str(g_img.width, nbuf); my_str_cat(stat_str, nbuf);
        my_str_cat(stat_str, "x");
        my_int_to_str(g_img.height, nbuf); my_str_cat(stat_str, nbuf);
        my_str_cat(stat_str, " 32-bit | Zoom: ");
        if (g_zoom_percent == 0) {
            my_str_cat(stat_str, "Fit");
        } else {
            my_int_to_str(g_zoom_percent, nbuf);
            my_str_cat(stat_str, nbuf);
            my_str_cat(stat_str, "%");
        }
        my_str_cat(stat_str, " | RAM: ");
        int mb_used = (int)((g_img.page_count * 4) / 1024);
        my_int_to_str(mb_used, nbuf); my_str_cat(stat_str, nbuf);
        my_str_cat(stat_str, "MB");
    } else {
        my_str_cat(stat_str, "No image loaded");
    }
    font_draw_text(v, wx + 8, sb_y + 3, stat_str, 0);

    // 3. Viewport area (between toolbar and status bar)
    int vp_x = wx + 1;
    int vp_y = tb_y + tb_h;
    int vp_w = ww - 2;
    int vp_h = sb_y - vp_y;

    if (vp_w <= 0 || vp_h <= 0) return;

    // Viewport background: dark slate gray
    for (int y = vp_y; y < vp_y + vp_h; y++) {
        for (int x = vp_x; x < vp_x + vp_w; x++) {
            PUT_PIXEL(v, x, y, 8);
        }
    }

    if (!g_img_loaded || !g_img.pixels || g_img.width <= 0 || g_img.height <= 0) {
        font_draw_text(v, vp_x + vp_w / 2 - 40, vp_y + vp_h / 2 - 8, "(No Image)", 15);
        return;
    }

    // Calculate display dimensions
    int dw, dh;
    if (g_zoom_percent == 0) { // Fit mode
        if ((unsigned int)g_img.width * (unsigned int)vp_h > (unsigned int)vp_w * (unsigned int)g_img.height) {
            dw = vp_w;
            dh = (int)(((unsigned int)g_img.height * (unsigned int)vp_w) / (unsigned int)g_img.width);
        } else {
            dh = vp_h;
            dw = (int)(((unsigned int)g_img.width * (unsigned int)vp_h) / (unsigned int)g_img.height);
        }
        if (dw <= 0) dw = 1;
        if (dh <= 0) dh = 1;
        g_pan_x = (vp_w - dw) / 2;
        g_pan_y = (vp_h - dh) / 2;
    } else {
        dw = (int)(((unsigned int)g_img.width * (unsigned int)g_zoom_percent) / 100);
        dh = (int)(((unsigned int)g_img.height * (unsigned int)g_zoom_percent) / 100);
        if (dw <= 0) dw = 1;
        if (dh <= 0) dh = 1;
    }

    int img_start_x = vp_x + g_pan_x;
    int img_start_y = vp_y + g_pan_y;

    // Clip rendering to viewport bounds
    int render_x0 = img_start_x < vp_x ? vp_x : img_start_x;
    int render_y0 = img_start_y < vp_y ? vp_y : img_start_y;
    int render_x1 = img_start_x + dw > vp_x + vp_w ? vp_x + vp_w : img_start_x + dw;
    int render_y1 = img_start_y + dh > vp_y + vp_h ? vp_y + vp_h : img_start_y + dh;

    // Render scaled image scanlines
    for (int y = render_y0; y < render_y1; y++) {
        int sy = (int)(((unsigned int)(y - img_start_y) * (unsigned int)g_img.height) / (unsigned int)dh);
        if (sy >= g_img.height) sy = g_img.height - 1;
        const unsigned int* src_row = &g_img.pixels[sy * g_img.width];

        for (int x = render_x0; x < render_x1; x++) {
            int sx = (int)(((unsigned int)(x - img_start_x) * (unsigned int)g_img.width) / (unsigned int)dw);
            if (sx >= g_img.width) sx = g_img.width - 1;
            unsigned int argb = src_row[sx];

            // Render pixel to screen backbuffer
            if (screen_bpp == 32) {
                *(unsigned int*)&v[y * screen_pitch + x * 4] = argb;
            } else if (screen_bpp == 24) {
                unsigned char* p = &v[y * screen_pitch + x * 3];
                p[0] = (unsigned char)(argb & 0xFF);
                p[1] = (unsigned char)((argb >> 8) & 0xFF);
                p[2] = (unsigned char)((argb >> 16) & 0xFF);
            } else if (screen_bpp == 16) {
                unsigned short c16 = (unsigned short)((((argb >> 16) & 0xF8) << 8) | (((argb >> 8) & 0xFC) << 3) | ((argb & 0xF8) >> 3));
                *(unsigned short*)&v[y * screen_pitch + x * 2] = c16;
            } else {
                unsigned char r = (argb >> 16) & 0xFF;
                unsigned char g = (argb >> 8) & 0xFF;
                unsigned char b = argb & 0xFF;
                unsigned char c = 0;
                if (r > 128) c |= 4;
                if (g > 128) c |= 2;
                if (b > 128) c |= 1;
                if (r > 200 || g > 200 || b > 200) c |= 8;
                v[y * screen_pitch + x] = c;
            }
        }
    }

    // 4. Wallpaper Dropdown Menu (if open)
    if (g_wp_menu_open) {
        int mx = g_wp_menu_x, my = g_wp_menu_y, mw = 110, mh = 4 * 18 + 4;
        for (int y = my; y < my + mh; y++) for (int x = mx; x < mx + mw; x++) PUT_PIXEL(v, x, y, 15);
        for (int x = mx; x < mx + mw; x++) { PUT_PIXEL(v, x, my, 0); PUT_PIXEL(v, x, my + mh - 1, 0); }
        for (int y = my; y < my + mh; y++) { PUT_PIXEL(v, mx, y, 0); PUT_PIXEL(v, mx + mw - 1, y, 0); }
        font_draw_text(v, mx + 8, my + 3 + 0 * 18, "1. Fill (填充)", 0);
        font_draw_text(v, mx + 8, my + 3 + 1 * 18, "2. Fit (适应)", 0);
        font_draw_text(v, mx + 8, my + 3 + 2 * 18, "3. Stretch (拉伸)", 0);
        font_draw_text(v, mx + 8, my + 3 + 3 * 18, "4. Tile (平铺)", 0);
    }

    // 5. OOM / Error Modal Dialog (if triggered)
    if (g_error_modal) {
        int dw = 280, dh = 90;
        int dx = wx + (ww - dw) / 2;
        int dy = wy + (wh - dh) / 2;
        for (int y = dy; y < dy + dh; y++) for (int x = dx; x < dx + dw; x++) PUT_PIXEL(v, x, y, 7);
        for (int x = dx; x < dx + dw; x++) { PUT_PIXEL(v, x, dy, 15); PUT_PIXEL(v, x, dy + dh - 1, 0); }
        for (int y = dy; y < dy + dh; y++) { PUT_PIXEL(v, dx, y, 15); PUT_PIXEL(v, dx + dw - 1, y, 0); }

        // Alert icon
        for (int y = dy + 10; y < dy + 34; y++) for (int x = dx + 12; x < dx + 36; x++) PUT_PIXEL(v, x, y, 4);
        font_draw_text(v, dx + 20, dy + 16, "!", 15);

        font_draw_text(v, dx + 44, dy + 12, g_err_title, 4);
        font_draw_text(v, dx + 44, dy + 28, g_err_msg, 0);
        font_draw_text(v, dx + 44, dy + 42, g_err_msg2, 0);

        draw_btn(v, dx + dw / 2 - 30, dy + 60, 60, 20, "OK");
    }
}

void imageviewer_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh) {
    (void)v;
    if (g_error_modal) {
        int dw = 280, dh = 90;
        int dx = wx + (ww - dw) / 2;
        int dy = wy + (wh - dh) / 2;
        int bx = dx + dw / 2 - 30, by = dy + 60;
        if (mx >= bx && mx < bx + 60 && my >= by && my < by + 20) {
            // Dismiss error and close app
            g_error_modal = 0;
            imv = 0;
            redraw = 1;
        }
        return;
    }

    if (g_wp_menu_open) {
        int px = g_wp_menu_x, py = g_wp_menu_y, pw = 110;
        if (mx >= px && mx < px + pw && my >= py && my < py + 4 * 18 + 4) {
            int item = (my - py - 2) / 18;
            if (item >= 0 && item < 4 && g_img_loaded) {
                wallpaper_mode_t m = (wallpaper_mode_t)item;
                wallpaper_set_image(&g_img, m);
            }
        }
        g_wp_menu_open = 0;
        redraw = 1;
        return;
    }

    int tb_y = wy + 19;
    // Check Toolbar clicks:
    // [ - ] Zoom Out
    if (mx >= wx + 8 && mx < wx + 34 && my >= tb_y + 3 && my < tb_y + 21) {
        if (g_zoom_percent == 0) g_zoom_percent = 100;
        for (int i = NUM_ZOOM_STEPS - 1; i >= 0; i--) {
            if (g_zoom_steps[i] < g_zoom_percent) {
                g_zoom_percent = g_zoom_steps[i];
                break;
            }
        }
        redraw = 1;
        return;
    }

    // [ + ] Zoom In
    if (mx >= wx + 38 && mx < wx + 64 && my >= tb_y + 3 && my < tb_y + 21) {
        if (g_zoom_percent == 0) g_zoom_percent = 50;
        for (int i = 0; i < NUM_ZOOM_STEPS; i++) {
            if (g_zoom_steps[i] > g_zoom_percent) {
                g_zoom_percent = g_zoom_steps[i];
                break;
            }
        }
        redraw = 1;
        return;
    }

    // [ 1:1 ] Original scale
    if (mx >= wx + 68 && mx < wx + 104 && my >= tb_y + 3 && my < tb_y + 21) {
        g_zoom_percent = 100;
        g_pan_x = 0; g_pan_y = 0;
        redraw = 1;
        return;
    }

    // [ Fit ] Fit to window
    if (mx >= wx + 108 && mx < wx + 144 && my >= tb_y + 3 && my < tb_y + 21) {
        g_zoom_percent = 0;
        g_pan_x = 0; g_pan_y = 0;
        redraw = 1;
        return;
    }

    // [ Set Wallpaper ]
    if (mx >= wx + 152 && mx < wx + 262 && my >= tb_y + 3 && my < tb_y + 21) {
        g_wp_menu_x = wx + 152;
        g_wp_menu_y = tb_y + 22;
        g_wp_menu_open = 1;
        redraw = 1;
        return;
    }

    // Start panning inside viewport
    int vp_y = tb_y + 24;
    int vp_h = (wy + wh - 18) - vp_y;
    if (mx >= wx + 1 && mx < wx + ww - 1 && my >= vp_y && my < vp_y + vp_h) {
        g_is_panning = 1;
        g_drag_last_x = mx;
        g_drag_last_y = my;
    }
}

void imageviewer_drag(int mx, int my, int wx, int wy, int ww, int wh) {
    (void)wx; (void)wy; (void)ww; (void)wh;
    if (g_is_panning) {
        int dx = mx - g_drag_last_x;
        int dy = my - g_drag_last_y;
        if (dx != 0 || dy != 0) {
            g_pan_x += dx;
            g_pan_y += dy;
            g_drag_last_x = mx;
            g_drag_last_y = my;
            redraw = 1;
        }
    }
}

void imageviewer_release(void) {
    g_is_panning = 0;
}

void imageviewer_handle_key(int key) {
    if (g_error_modal) {
        if (key == 10 || key == 13 || key == 27) { // Enter or Esc
            g_error_modal = 0;
            imv = 0;
            redraw = 1;
        }
        return;
    }
    if (key == '+' || key == '=') {
        if (g_zoom_percent == 0) g_zoom_percent = 50;
        for (int i = 0; i < NUM_ZOOM_STEPS; i++) {
            if (g_zoom_steps[i] > g_zoom_percent) {
                g_zoom_percent = g_zoom_steps[i];
                break;
            }
        }
        redraw = 1;
    } else if (key == '-' || key == '_') {
        if (g_zoom_percent == 0) g_zoom_percent = 100;
        for (int i = NUM_ZOOM_STEPS - 1; i >= 0; i--) {
            if (g_zoom_steps[i] < g_zoom_percent) {
                g_zoom_percent = g_zoom_steps[i];
                break;
            }
        }
        redraw = 1;
    } else if (key == '0') {
        g_zoom_percent = 0; // Fit
        g_pan_x = 0; g_pan_y = 0;
        redraw = 1;
    } else if (key == '1') {
        g_zoom_percent = 100; // 1:1
        g_pan_x = 0; g_pan_y = 0;
        redraw = 1;
    }
}
