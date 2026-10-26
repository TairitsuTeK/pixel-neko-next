#include "gfx_accel.h"
#include "ati_mach64.h"
#include "vmware_svga.h"
#include "driver.h"
#include "log.h"
#include "irq.h"

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char* lfb;
extern unsigned int lfb_ptr;

typedef struct {
    const char* name;
    int kind;
    void (*present_rect)(int x, int y, int width, int height);
    void (*fill_rect)(int x, int y, int width, int height,
                      unsigned int color);
    void (*copy_rect)(int sx, int sy, int dx, int dy, int width, int height);
    void (*sync)(void);
} gfx_backend_t;

#define GFX_BACKEND_SOFTWARE 0
#define GFX_BACKEND_VMWARE   1
#define GFX_BACKEND_ATI      2

static int g_initialized;
static int g_drivers_registered;
static int g_backend_kind = GFX_BACKEND_SOFTWARE;
static const gfx_backend_t* g_backend;
static const char* g_fallback_reason = "No hardware 2D backend selected";

static void backend_svga_present(int x, int y, int width, int height);
static void backend_svga_fill(int x, int y, int width, int height,
                              unsigned int color);
static void backend_svga_copy(int sx, int sy, int dx, int dy,
                              int width, int height);
static void backend_ati_present(int x, int y, int width, int height);
static void backend_ati_fill(int x, int y, int width, int height,
                             unsigned int color);
static void backend_ati_copy(int sx, int sy, int dx, int dy,
                             int width, int height);
static void backend_software_present(int x, int y, int width, int height);
static void backend_software_fill(int x, int y, int width, int height,
                                  unsigned int color);
static void backend_software_copy(int sx, int sy, int dx, int dy,
                                  int width, int height);

static const gfx_backend_t g_svga_backend = {
    "VMware SVGA II",
    GFX_BACKEND_VMWARE,
    backend_svga_present,
    backend_svga_fill,
    backend_svga_copy,
    vmware_svga_sync
};

static const gfx_backend_t g_ati_backend = {
    "ATI Mach64",
    GFX_BACKEND_ATI,
    backend_ati_present,
    backend_ati_fill,
    backend_ati_copy,
    ati_mach64_sync
};

static const gfx_backend_t g_software_backend = {
    "Software VBE",
    GFX_BACKEND_SOFTWARE,
    backend_software_present,
    backend_software_fill,
    backend_software_copy,
    0
};

static int probe_vmware_svga(void)
{
    if (!vmware_svga_probe()) return 0;
    int bpp = 32;
    if (!vmware_svga_set_mode(screen_w, screen_h, bpp)) return 0;

    screen_bpp = bpp;
    screen_pitch = vmware_svga_get_pitch();
    lfb_ptr = vmware_svga_get_framebuffer();
    lfb = (unsigned char*)lfb_ptr;
    return 1;
}

static int probe_ati_mach64(void)
{
    if (!ati_mach64_probe()) return 0;
    ati_mach64_set_mode_bpp(screen_w, screen_h, screen_bpp);
    return 1;
}

static const driver_descriptor_t g_vmware_driver = {
    "VMware SVGA II",
    DRIVER_CATEGORY_DISPLAY,
    100,
    probe_vmware_svga,
    0,
    0
};

static const driver_descriptor_t g_ati_driver = {
    "ATI Mach64",
    DRIVER_CATEGORY_DISPLAY,
    50,
    probe_ati_mach64,
    0,
    0
};

static void register_gfx_drivers(void)
{
    if (g_drivers_registered) return;
    driver_register(&g_vmware_driver);
    driver_register(&g_ati_driver);
    g_drivers_registered = 1;
}

void gfx_accel_init(void)
{
    if (g_initialized) return;

    register_gfx_drivers();
    g_backend = &g_software_backend;
    g_backend_kind = GFX_BACKEND_SOFTWARE;

    if (driver_probe_category(DRIVER_CATEGORY_DISPLAY)) {
        if (driver_is_active(DRIVER_CATEGORY_DISPLAY, "VMware SVGA II")) {
            g_backend = &g_svga_backend;
            g_backend_kind = GFX_BACKEND_VMWARE;
            g_fallback_reason = "None";
        } else if (driver_is_active(DRIVER_CATEGORY_DISPLAY, "ATI Mach64")) {
            g_backend = &g_ati_backend;
            g_backend_kind = GFX_BACKEND_ATI;
            g_fallback_reason = "None";
        }
    }

    g_initialized = 1;
    if (g_backend_kind == GFX_BACKEND_SOFTWARE) {
        g_fallback_reason = "VMware SVGA II and ATI Mach64 unavailable";
        log_printf("[GFX ] falling back to software VBE backend\n");
    } else {
        log_printf("[GFX ] active backend: %s, status: %s\n",
                   gfx_accel_get_name(), gfx_accel_get_status());
    }
}

int gfx_accel_is_active(void)
{
    return g_initialized && g_backend_kind != GFX_BACKEND_SOFTWARE;
}

const char* gfx_accel_get_name(void)
{
    if (!g_initialized || !g_backend) return "Standard VBE (Not probed)";
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_get_name();
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        return ati_mach64_get_chip_name();
    }
    return "Standard VBE (Software)";
}

const char* gfx_accel_get_status(void)
{
    if (!g_initialized || !g_backend) return "Graphics probe not initialized";
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_get_status();
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        return "GUI accelerator active";
    }
    return g_fallback_reason;
}

const char* gfx_accel_get_fallback_reason(void)
{
    return g_fallback_reason;
}

unsigned int gfx_accel_get_vram_size(void)
{
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_get_vram_size();
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        return ati_mach64_get_vram_size();
    }
    return 2 * 1024 * 1024;
}

static int vbe_mode_available(int w, int h, int bpp)
{
    extern int vbe_find_mode(int w, int h, int bpp);
    irq_bios_state_t state;
    irq_bios_suspend(&state);
    int result = vbe_find_mode(w, h, bpp) > 0;
    irq_bios_resume(&state);
    return result;
}

int gfx_accel_supports_bpp(int w, int h, int bpp)
{
    unsigned int bytes_per_pixel =
        (bpp <= 8) ? 1 : ((bpp <= 16) ? 2 : ((bpp <= 24) ? 3 : 4));
    unsigned int vram_needed =
        (unsigned int)w * (unsigned int)h * bytes_per_pixel;
    unsigned int max_vram = gfx_accel_get_vram_size();
    if (w <= 0 || h <= 0 || vram_needed > max_vram) return 0;

    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_supports_mode(w, h, bpp);
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        if (bpp == 32) {
            if (vbe_mode_available(w, h, 32)) return 1;
            return ati_mach64_supports_native_32bpp(w, h) &&
                   vbe_mode_available(w, h, 24);
        }
        return bpp == 8 || bpp == 15 || bpp == 16 ||
               bpp == 24 || bpp == 32;
    }
    return bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32;
}

int gfx_accel_try_set_mode(int w, int h, int bpp)
{
    if (g_backend_kind != GFX_BACKEND_VMWARE) return 0;
    if (!vmware_svga_set_mode(w, h, bpp)) {
        driver_deactivate(DRIVER_CATEGORY_DISPLAY);
        g_backend = &g_software_backend;
        g_backend_kind = GFX_BACKEND_SOFTWARE;
        g_fallback_reason = "VMware SVGA mode switch failed; VBE fallback";
        log_printf("[GFX ] %s\n", g_fallback_reason);
        return 0;
    }
    screen_w = w;
    screen_h = h;
    screen_bpp = bpp;
    screen_pitch = vmware_svga_get_pitch();
    lfb_ptr = vmware_svga_get_framebuffer();
    lfb = (unsigned char*)lfb_ptr;
    return 1;
}

void gfx_accel_set_mode(int w, int h, int bpp)
{
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        vmware_svga_set_mode(w, h, bpp);
    } else if (g_backend_kind == GFX_BACKEND_ATI) {
        ati_mach64_set_mode_bpp(w, h, bpp);
    }
}

void gfx_accel_fill_rect(int x, int y, int w, int h, unsigned int color)
{
    if (g_backend && g_backend->fill_rect) {
        g_backend->fill_rect(x, y, w, h, color);
    }
}

void gfx_accel_screen_blt(int sx, int sy, int dx, int dy, int w, int h)
{
    if (g_backend && g_backend->copy_rect) {
        g_backend->copy_rect(sx, sy, dx, dy, w, h);
    }
}

void gfx_accel_present_rect(int x, int y, int w, int h)
{
    if (g_backend && g_backend->present_rect) {
        g_backend->present_rect(x, y, w, h);
    }
}

void gfx_accel_sync(void)
{
    if (g_backend && g_backend->sync) g_backend->sync();
}

int gfx_accel_has_hardware_cursor(void)
{
    return g_initialized && g_backend_kind == GFX_BACKEND_VMWARE &&
           vmware_svga_cursor_ready();
}

int gfx_accel_set_cursor(int x, int y, int visible)
{
    if (g_backend_kind != GFX_BACKEND_VMWARE) return 0;
    return vmware_svga_set_cursor(x, y, visible);
}

static void backend_svga_present(int x, int y, int width, int height)
{
    vmware_svga_present_rect(x, y, width, height);
}

static void backend_svga_fill(int x, int y, int width, int height,
                              unsigned int color)
{
    vmware_svga_fill_rect(x, y, width, height, color);
}

static void backend_svga_copy(int sx, int sy, int dx, int dy,
                              int width, int height)
{
    vmware_svga_screen_blt(sx, sy, dx, dy, width, height);
}

static void backend_ati_present(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void backend_ati_fill(int x, int y, int width, int height,
                             unsigned int color)
{
    ati_mach64_fill_rect(x, y, width, height, color);
}

static void backend_ati_copy(int sx, int sy, int dx, int dy,
                             int width, int height)
{
    ati_mach64_screen_blt(sx, sy, dx, dy, width, height);
}

static void backend_software_present(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void backend_software_fill(int x, int y, int width, int height,
                                  unsigned int color)
{
    if (!lfb) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > screen_w) width = screen_w - x;
    if (y + height > screen_h) height = screen_h - y;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = (screen_bpp <= 8) ? 1 :
                         ((screen_bpp <= 16) ? 2 :
                         ((screen_bpp <= 24) ? 3 : 4));
    for (int row = y; row < y + height; row++) {
        unsigned char* destination = lfb + row * screen_pitch + x * bytes;
        if (bytes == 4) {
            unsigned int* p = (unsigned int*)destination;
            for (int column = 0; column < width; column++) p[column] = color;
        } else if (bytes == 2) {
            unsigned short* p = (unsigned short*)destination;
            for (int column = 0; column < width; column++) {
                p[column] = (unsigned short)color;
            }
        } else if (bytes == 3) {
            for (int column = 0; column < width; column++) {
                destination[column * 3] = (unsigned char)color;
                destination[column * 3 + 1] = (unsigned char)(color >> 8);
                destination[column * 3 + 2] = (unsigned char)(color >> 16);
            }
        } else {
            for (int column = 0; column < width; column++) {
                destination[column] = (unsigned char)color;
            }
        }
    }
}

static void backend_software_copy(int sx, int sy, int dx, int dy,
                                  int width, int height)
{
    if (!lfb) return;
    if (sx < 0) { width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { height += sy; dy -= sy; sy = 0; }
    if (dx < 0) { width += dx; sx -= dx; dx = 0; }
    if (dy < 0) { height += dy; sy -= dy; dy = 0; }
    if (sx + width > screen_w) width = screen_w - sx;
    if (sy + height > screen_h) height = screen_h - sy;
    if (dx + width > screen_w) width = screen_w - dx;
    if (dy + height > screen_h) height = screen_h - dy;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = (screen_bpp <= 8) ? 1 :
                         ((screen_bpp <= 16) ? 2 :
                         ((screen_bpp <= 24) ? 3 : 4));
    int row_start = height - 1;
    int row_end = -1;
    int row_step = -1;
    if (dy < sy) {
        row_start = 0;
        row_end = height;
        row_step = 1;
    }

    for (int row = row_start; row != row_end; row += row_step) {
        unsigned char* source = lfb + (sy + row) * screen_pitch + sx * bytes;
        unsigned char* destination =
            lfb + (dy + row) * screen_pitch + dx * bytes;
        int line_bytes = width * (int)bytes;
        for (int i = 0; i < line_bytes; i++) destination[i] = source[i];
    }
}
