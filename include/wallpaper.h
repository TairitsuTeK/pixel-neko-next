#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "image_dec.h"

typedef enum {
    WP_MODE_FILL = 0,    // 填充
    WP_MODE_FIT = 1,     // 适应
    WP_MODE_STRETCH = 2, // 拉伸
    WP_MODE_TILE = 3     // 平铺
} wallpaper_mode_t;

void wallpaper_init(void);
int  wallpaper_set_image(const image_t* img, wallpaper_mode_t mode);
void wallpaper_set_mode(wallpaper_mode_t mode);
wallpaper_mode_t wallpaper_get_mode(void);
int  wallpaper_is_active(void);
void wallpaper_clear(void);
const char* wallpaper_get_mode_name(wallpaper_mode_t mode);

// Render wallpaper into desktop backbuffer
void wallpaper_render(unsigned char* backbuf, int sw, int sh, int sbpp, int spitch);

#endif // WALLPAPER_H
