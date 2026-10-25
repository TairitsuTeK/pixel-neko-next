#include "gfx_accel.h"
#include "ati_mach64.h"

extern int screen_w, screen_h, screen_bpp;
extern unsigned char* lfb;

static int g_accel_initialized = 0;

void gfx_accel_init(void) {
    if (g_accel_initialized) return;

    // Probe for hardware 2D accelerator (ATI Mach64 / 3D Rage II+ family)
    if (ati_mach64_probe()) {
        ati_mach64_set_mode_bpp(screen_w, screen_h, screen_bpp);
    }
    g_accel_initialized = 1;
}

int gfx_accel_is_active(void) {
    return ati_mach64_is_active();
}

const char* gfx_accel_get_name(void) {
    if (ati_mach64_is_active()) {
        return ati_mach64_get_chip_name();
    }
    return "Standard VBE (Software)";
}

unsigned int gfx_accel_get_vram_size(void) {
    if (ati_mach64_is_active()) {
        return ati_mach64_get_vram_size();
    }
    return 2 * 1024 * 1024; // Standard 2MB VBE fallback
}

int gfx_accel_supports_bpp(int w, int h, int bpp) {
    unsigned int bytes_per_pixel = (bpp <= 8) ? 1 : ((bpp <= 16) ? 2 : ((bpp <= 24) ? 3 : 4));
    unsigned int vram_needed = (unsigned int)w * (unsigned int)h * bytes_per_pixel;
    unsigned int max_vram = gfx_accel_get_vram_size();

    // VRAM physical limit check
    if (vram_needed > max_vram) {
        return 0; // Exceeds physical VRAM!
    }

    if (ati_mach64_is_active()) {
        if (bpp == 32) {
            extern int vbe_find_mode(int w, int h, int bpp);
            if (vbe_find_mode(w, h, 32) > 0) return 1;
            return ati_mach64_supports_native_32bpp(w, h) &&
                   vbe_find_mode(w, h, 24) > 0;
        }
        return (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32);
    }

    return (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32);
}

void gfx_accel_set_mode(int w, int h, int bpp) {
    if (ati_mach64_is_active()) {
        ati_mach64_set_mode_bpp(w, h, bpp);
    }
}

void gfx_accel_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (ati_mach64_is_active()) {
        ati_mach64_fill_rect(x, y, w, h, color);
        return;
    }

    // Software fallback fill
    if (!lfb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;

    for (int r = y; r < y + h; r++) {
        unsigned char* dst = lfb + (r * screen_w + x);
        for (int c = 0; c < w; c++) {
            dst[c] = (unsigned char)color;
        }
    }
}

void gfx_accel_screen_blt(int sx, int sy, int dx, int dy, int w, int h) {
    if (ati_mach64_is_active()) {
        ati_mach64_screen_blt(sx, sy, dx, dy, w, h);
        return;
    }

    // Software fallback blit
    if (!lfb) return;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > screen_w) w = screen_w - sx;
    if (sy + h > screen_h) h = screen_h - sy;
    if (dx + w > screen_w) w = screen_w - dx;
    if (dy + h > screen_h) h = screen_h - dy;
    if (w <= 0 || h <= 0) return;

    if (dy > sy) {
        for (int r = h - 1; r >= 0; r--) {
            unsigned char* src = lfb + ((sy + r) * screen_w + sx);
            unsigned char* dst = lfb + ((dy + r) * screen_w + dx);
            for (int c = 0; c < w; c++) dst[c] = src[c];
        }
    } else {
        for (int r = 0; r < h; r++) {
            unsigned char* src = lfb + ((sy + r) * screen_w + sx);
            unsigned char* dst = lfb + ((dy + r) * screen_w + dx);
            for (int c = 0; c < w; c++) dst[c] = src[c];
        }
    }
}

void gfx_accel_sync(void) {
    if (ati_mach64_is_active()) {
        ati_mach64_sync();
    }
}
