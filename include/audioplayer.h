#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

void audioplayer_init(void);
void audioplayer_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void audioplayer_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh);
void audioplayer_drag(int mx, int my, int wx, int wy, int ww, int wh);
void audioplayer_release(void);
void audioplayer_open_file(const char* filepath);
const char* audioplayer_get_title(void);

#endif
