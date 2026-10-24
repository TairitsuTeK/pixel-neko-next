#ifndef IMAGEVIEWER_H
#define IMAGEVIEWER_H

extern int imv; // Visible
extern int imm; // Minimized

void imageviewer_init(void);
void imageviewer_show_sample(void);
void imageviewer_open_file(const char* path);
void imageviewer_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void imageviewer_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh);
void imageviewer_drag(int mx, int my, int wx, int wy, int ww, int wh);
void imageviewer_release(void);
void imageviewer_handle_key(int key);
const char* imageviewer_get_title(void);

#endif // IMAGEVIEWER_H
