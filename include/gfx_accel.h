#ifndef GFX_ACCEL_H
#define GFX_ACCEL_H

// ----------------------------------------------------------------------------
// Unified 2D graphics backend HAL. Drivers remain in ring0.
// ----------------------------------------------------------------------------

void         gfx_accel_init(void);
int          gfx_accel_is_active(void);
const char*  gfx_accel_get_name(void);
const char*  gfx_accel_get_status(void);
const char*  gfx_accel_get_fallback_reason(void);
unsigned int gfx_accel_get_vram_size(void);
int          gfx_accel_supports_bpp(int w, int h, int bpp);
int          gfx_accel_try_set_mode(int w, int h, int bpp);
void         gfx_accel_set_mode(int w, int h, int bpp);
void         gfx_accel_fill_rect(int x, int y, int w, int h, unsigned int color);
void         gfx_accel_screen_blt(int sx, int sy, int dx, int dy, int w, int h);
void         gfx_accel_present_rect(int x, int y, int w, int h);
void         gfx_accel_sync(void);
int          gfx_accel_has_hardware_cursor(void);
int          gfx_accel_set_cursor(int x, int y, int visible);

#endif
