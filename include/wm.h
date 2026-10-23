#ifndef WM_H
#define WM_H

void wm_init(void);
void wm_update(unsigned char* v, int mx, int my, int mb, int mb_press, int mb_release);
void wm_draw(unsigned char* v);
int wm_is_dragging(void);
int wm_need_mouse_reset(void);

void copy_fullscreen_to_lfb(const unsigned char* s_buf);
void copy_rect_to_lfb(const unsigned char* src, int rx, int ry, int rw, int rh);

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char backbuffer[];

extern unsigned short palette16[256];

#define RGB16(r, g, b) (unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

#define PUT_PIXEL(v, px, py, c) do { \
    if ((px) >= 0 && (px) < screen_w && (py) >= 0 && (py) < screen_h) { \
        if (screen_bpp == 8) (v)[(py)*screen_pitch + (px)] = (unsigned char)(c); \
        else *(unsigned short*)&((v)[(py)*screen_pitch + (px)*2]) = palette16[(unsigned char)(c)]; \
    } \
} while(0)

#endif