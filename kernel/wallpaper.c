#include "wallpaper.h"
#include "mm.h"

static int g_wp_active = 0;
static wallpaper_mode_t g_wp_mode = WP_MODE_FILL;
static int g_wp_width = 0;
static int g_wp_height = 0;
static unsigned int* g_wp_pixels = 0;
static unsigned int g_wp_pages = 0;

void wallpaper_init(void) {
    g_wp_active = 0;
    g_wp_mode = WP_MODE_FILL;
    g_wp_width = 0;
    g_wp_height = 0;
    g_wp_pixels = 0;
    g_wp_pages = 0;
}

int wallpaper_is_active(void) {
    return g_wp_active && g_wp_pixels != 0;
}

wallpaper_mode_t wallpaper_get_mode(void) {
    return g_wp_mode;
}

void wallpaper_set_mode(wallpaper_mode_t mode) {
    g_wp_mode = mode;
}

const char* wallpaper_get_mode_name(wallpaper_mode_t mode) {
    switch (mode) {
        case WP_MODE_FILL: return "Fill";
        case WP_MODE_FIT: return "Fit";
        case WP_MODE_STRETCH: return "Stretch";
        case WP_MODE_TILE: return "Tile";
        default: return "Unknown";
    }
}

void wallpaper_clear(void) {
    if (g_wp_pixels && g_wp_pages > 0) {
        pmm_free_pages(g_wp_pixels, g_wp_pages);
        g_wp_pixels = 0;
        g_wp_pages = 0;
    }
    g_wp_active = 0;
    g_wp_width = 0;
    g_wp_height = 0;
}

int wallpaper_set_image(const image_t* img, wallpaper_mode_t mode) {
    if (!img || !img->pixels || img->width <= 0 || img->height <= 0) {
        return -1;
    }

    unsigned int total_pixels = (unsigned int)img->width * (unsigned int)img->height;
    unsigned int bytes = total_pixels * 4;
    unsigned int pages_needed = (bytes + 4095) / 4096;

    if (pages_needed > pmm_get_free_pages()) {
        return -2; // Not enough memory
    }

    unsigned int* new_buf = (unsigned int*)pmm_alloc_pages(pages_needed);
    if (!new_buf) return -2;

    // Free existing wallpaper
    wallpaper_clear();

    // Copy pixels
    for (unsigned int i = 0; i < total_pixels; i++) {
        new_buf[i] = img->pixels[i];
    }

    g_wp_pixels = new_buf;
    g_wp_pages = pages_needed;
    g_wp_width = img->width;
    g_wp_height = img->height;
    g_wp_mode = mode;
    g_wp_active = 1;

    return 0;
}

static inline void put_wp_pixel(unsigned char* backbuf, int x, int y, unsigned int argb, int bpp, int pitch) {
    if (bpp == 32) {
        *(unsigned int*)&backbuf[y * pitch + x * 4] = argb;
    } else if (bpp == 24) {
        unsigned char* p = &backbuf[y * pitch + x * 3];
        p[0] = (unsigned char)(argb & 0xFF);
        p[1] = (unsigned char)((argb >> 8) & 0xFF);
        p[2] = (unsigned char)((argb >> 16) & 0xFF);
    } else if (bpp == 16) {
        unsigned short c16 = (unsigned short)((((argb >> 16) & 0xF8) << 8) | (((argb >> 8) & 0xFC) << 3) | ((argb & 0xF8) >> 3));
        *(unsigned short*)&backbuf[y * pitch + x * 2] = c16;
    } else { // 8-bit palette mode
        unsigned char r = (argb >> 16) & 0xFF;
        unsigned char g = (argb >> 8) & 0xFF;
        unsigned char b = argb & 0xFF;
        unsigned char c = 0;
        if (r > 128) c |= 4;
        if (g > 128) c |= 2;
        if (b > 128) c |= 1;
        if (r > 200 || g > 200 || b > 200) c |= 8;
        backbuf[y * pitch + x] = c;
    }
}

void wallpaper_render(unsigned char* backbuf, int sw, int sh, int sbpp, int spitch) {
    if (!g_wp_active || !g_wp_pixels || g_wp_width <= 0 || g_wp_height <= 0) return;

    int iw = g_wp_width;
    int ih = g_wp_height;

    if (g_wp_mode == WP_MODE_STRETCH) {
        for (int y = 0; y < sh; y++) {
            int sy = (y * ih) / sh;
            const unsigned int* src_row = &g_wp_pixels[sy * iw];
            for (int x = 0; x < sw; x++) {
                int sx = (x * iw) / sw;
                put_wp_pixel(backbuf, x, y, src_row[sx], sbpp, spitch);
            }
        }
    } else if (g_wp_mode == WP_MODE_TILE) {
        for (int y = 0; y < sh; y++) {
            int sy = y % ih;
            const unsigned int* src_row = &g_wp_pixels[sy * iw];
            for (int x = 0; x < sw; x++) {
                int sx = x % iw;
                put_wp_pixel(backbuf, x, y, src_row[sx], sbpp, spitch);
            }
        }
    } else if (g_wp_mode == WP_MODE_FILL) {
        // Fill: maintain aspect ratio, cover whole screen, crop excess
        if ((unsigned int)iw * (unsigned int)sh > (unsigned int)sw * (unsigned int)ih) {
            // Image is wider than screen: scale to height, crop X
            int target_h = sh;
            int target_w = (int)(((unsigned int)iw * (unsigned int)sh) / (unsigned int)ih);
            int crop_x = (target_w - sw) / 2;
            for (int y = 0; y < sh; y++) {
                int sy = (y * ih) / target_h;
                const unsigned int* src_row = &g_wp_pixels[sy * iw];
                for (int x = 0; x < sw; x++) {
                    int sx = (int)(((unsigned int)(x + crop_x) * (unsigned int)iw) / (unsigned int)target_w);
                    if (sx < 0) sx = 0;
                    if (sx >= iw) sx = iw - 1;
                    put_wp_pixel(backbuf, x, y, src_row[sx], sbpp, spitch);
                }
            }
        } else {
            // Image is taller than screen: scale to width, crop Y
            int target_w = sw;
            int target_h = (int)(((unsigned int)ih * (unsigned int)sw) / (unsigned int)iw);
            int crop_y = (target_h - sh) / 2;
            for (int y = 0; y < sh; y++) {
                int sy = (int)(((unsigned int)(y + crop_y) * (unsigned int)ih) / (unsigned int)target_h);
                if (sy < 0) sy = 0;
                if (sy >= ih) sy = ih - 1;
                const unsigned int* src_row = &g_wp_pixels[sy * iw];
                for (int x = 0; x < sw; x++) {
                    int sx = (x * iw) / target_w;
                    put_wp_pixel(backbuf, x, y, src_row[sx], sbpp, spitch);
                }
            }
        }
    } else if (g_wp_mode == WP_MODE_FIT) {
        // Fit: maintain aspect ratio, entire image visible, letterbox / black bars
        int dw, dh, dx, dy;
        if ((unsigned int)iw * (unsigned int)sh > (unsigned int)sw * (unsigned int)ih) {
            // Wider: full width, black bars top/bottom
            dw = sw;
            dh = (int)(((unsigned int)ih * (unsigned int)sw) / (unsigned int)iw);
            dx = 0;
            dy = (sh - dh) / 2;
        } else {
            // Taller: full height, black bars left/right
            dh = sh;
            dw = (int)(((unsigned int)iw * (unsigned int)sh) / (unsigned int)ih);
            dy = 0;
            dx = (sw - dw) / 2;
        }

        for (int y = 0; y < sh; y++) {
            if (y < dy || y >= dy + dh) {
                for (int x = 0; x < sw; x++) put_wp_pixel(backbuf, x, y, 0xFF000000, sbpp, spitch);
            } else {
                int sy = ((y - dy) * ih) / dh;
                const unsigned int* src_row = &g_wp_pixels[sy * iw];
                for (int x = 0; x < sw; x++) {
                    if (x < dx || x >= dx + dw) {
                        put_wp_pixel(backbuf, x, y, 0xFF000000, sbpp, spitch);
                    } else {
                        int sx = ((x - dx) * iw) / dw;
                        put_wp_pixel(backbuf, x, y, src_row[sx], sbpp, spitch);
                    }
                }
            }
        }
    }
}
