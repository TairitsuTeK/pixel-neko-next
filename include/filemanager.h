#ifndef FILEMANAGER_H
#define FILEMANAGER_H

void fm_init(void);
void fm_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void fm_update(unsigned char* v, int mx, int my, int mb, int wx, int wy, int ww, int wh);
void fm_handle_key(int key);
void fm_release(void);

#endif