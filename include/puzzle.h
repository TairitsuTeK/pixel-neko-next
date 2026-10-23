#ifndef PUZZLE_H
#define PUZZLE_H

void puzzle_init(void);
void puzzle_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void puzzle_click(int mx, int my, int wx, int wy, int ww, int wh);

#endif
