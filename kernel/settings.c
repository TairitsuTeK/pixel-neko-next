#include "settings.h"
#include "vfs.h"
#include "fat.h"
#include "image_dec.h"
#include "wallpaper.h"

extern int screen_w, screen_h, screen_bpp;
extern void set_resolution(int w, int h, int bpp);
extern void gfx_accel_set_mode(int w, int h, int bpp);
extern int vbe_supports_mode(int w, int h, int bpp);
extern void enable_fpu(void);
extern void timer_init(void);
extern int g_desktop_pattern;

static int g_saved_res_w = 800;
static int g_saved_res_h = 600;
static int g_saved_bpp = 16;
static int g_saved_pattern = 0;
static wallpaper_mode_t g_saved_wp_mode = WP_MODE_FILL;
static char g_saved_wp_path[128] = "";

static int str_cmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char*)a - *(const unsigned char*)b;
}

static int parse_int(const char* s) {
    int res = 0;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res;
}

void settings_init(void) {
    wallpaper_init();
    g_saved_res_w = screen_w > 0 ? screen_w : 800;
    g_saved_res_h = screen_h > 0 ? screen_h : 600;
    g_saved_bpp = screen_bpp > 0 ? screen_bpp : 16;
    g_saved_pattern = g_desktop_pattern;
    g_saved_wp_mode = wallpaper_get_mode();
    g_saved_wp_path[0] = '\0';
}

void settings_set_resolution(int w, int h, int bpp) {
    g_saved_res_w = w;
    g_saved_res_h = h;
    g_saved_bpp = bpp;
}

void settings_set_pattern(int pattern) {
    g_saved_pattern = pattern;
}

void settings_set_wallpaper(const char* path, wallpaper_mode_t mode) {
    int i = 0;
    while (path && path[i] && i < 127) {
        g_saved_wp_path[i] = path[i];
        i++;
    }
    g_saved_wp_path[i] = '\0';
    g_saved_wp_mode = mode;
}

void settings_clear_wallpaper(void) {
    g_saved_wp_path[0] = '\0';
}

void settings_save(void) {
    if (vfs_is_booted_from_cdrom()) {
        return; // Live CD mode, do not write persistent settings to disk
    }

    char buf[256];
    int pos = 0;

    #define APPEND_STR(s) do { const char* _p = (s); while (*_p && pos < 250) buf[pos++] = *_p++; } while(0)
    #define APPEND_INT(n) do { \
        char _t[16]; int _tp = 0; int _v = (n); \
        if (_v == 0) buf[pos++] = '0'; \
        else { while (_v > 0) { _t[_tp++] = '0' + (_v % 10); _v /= 10; } \
               while (_tp > 0 && pos < 250) buf[pos++] = _t[--_tp]; } \
    } while(0)

    APPEND_STR("res_w="); APPEND_INT(g_saved_res_w); APPEND_STR("\r\n");
    APPEND_STR("res_h="); APPEND_INT(g_saved_res_h); APPEND_STR("\r\n");
    APPEND_STR("bpp="); APPEND_INT(g_saved_bpp); APPEND_STR("\r\n");
    APPEND_STR("pattern="); APPEND_INT(g_saved_pattern); APPEND_STR("\r\n");
    APPEND_STR("wp_mode="); APPEND_INT((int)g_saved_wp_mode); APPEND_STR("\r\n");
    APPEND_STR("wallpaper="); APPEND_STR(g_saved_wp_path); APPEND_STR("\r\n");

    buf[pos] = '\0';
    vfs_write_file("C:\\settings.ini", (const unsigned char*)buf, (unsigned int)pos);
    fat_flush_all();
}

void settings_load(void) {
    if (vfs_is_booted_from_cdrom()) {
        return; // Live CD mode
    }

    unsigned char buf[512];
    int bytes = vfs_read_file("C:\\settings.ini", buf, sizeof(buf) - 1);
    if (bytes <= 0) return;
    buf[bytes] = 0;

    char* line = (char*)buf;
    while (*line) {
        while (*line == '\r' || *line == '\n' || *line == ' ') line++;
        if (!*line) break;

        char* next_line = line;
        while (*next_line && *next_line != '\r' && *next_line != '\n') next_line++;
        if (*next_line) { *next_line = '\0'; next_line++; }

        char* eq = line;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            char* key = line;
            char* val = eq + 1;

            if (str_cmp(key, "res_w") == 0) g_saved_res_w = parse_int(val);
            else if (str_cmp(key, "res_h") == 0) g_saved_res_h = parse_int(val);
            else if (str_cmp(key, "bpp") == 0) g_saved_bpp = parse_int(val);
            else if (str_cmp(key, "pattern") == 0) g_saved_pattern = parse_int(val);
            else if (str_cmp(key, "wp_mode") == 0) g_saved_wp_mode = (wallpaper_mode_t)parse_int(val);
            else if (str_cmp(key, "wallpaper") == 0) {
                int k = 0; while (val[k] && k < 127) { g_saved_wp_path[k] = val[k]; k++; }
                g_saved_wp_path[k] = '\0';
            }
        }
        line = next_line;
    }

    if (g_saved_res_w > 0 && g_saved_res_h > 0 && g_saved_bpp > 0) {
        if (g_saved_res_w != screen_w || g_saved_res_h != screen_h || g_saved_bpp != screen_bpp) {
            if (vbe_supports_mode(g_saved_res_w, g_saved_res_h, g_saved_bpp)) {
                set_resolution(g_saved_res_w, g_saved_res_h, g_saved_bpp);
                gfx_accel_set_mode(g_saved_res_w, g_saved_res_h, g_saved_bpp);
                enable_fpu();
                timer_init();
            }
        }
    }

    if (g_saved_pattern >= 0 && g_saved_pattern < 6) {
        g_desktop_pattern = g_saved_pattern;
    }

    if (g_saved_wp_path[0]) {
        image_t img;
        image_init(&img);
        char err[64];
        if (image_load_from_file(g_saved_wp_path, &img, err, sizeof(err)) == 0) {
            wallpaper_set_image(&img, g_saved_wp_mode);
            image_free(&img);
        }
    }
}
