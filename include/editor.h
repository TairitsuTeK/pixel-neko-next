#ifndef EDITOR_H
#define EDITOR_H

void editor_init(void);
void editor_open_file(const char* filename);
void editor_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void editor_click(int mx, int my, int wx, int wy, int ww, int wh);
void editor_drag(int mx, int my, int wx, int wy, int ww, int wh);
void editor_release(void);
void editor_handle_key(int key);
void editor_cut(void);
void editor_copy(void);
void editor_paste(void);

#endif
