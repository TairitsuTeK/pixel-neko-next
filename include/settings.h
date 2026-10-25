#ifndef SETTINGS_H
#define SETTINGS_H

#include "wallpaper.h"

void settings_init(void);
void settings_load(void);
void settings_save(void);
void settings_set_resolution(int w, int h, int bpp);
void settings_set_pattern(int pattern);
void settings_set_wallpaper(const char* path, wallpaper_mode_t mode);
void settings_clear_wallpaper(void);

#endif // SETTINGS_H
