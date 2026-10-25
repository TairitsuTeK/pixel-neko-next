#ifndef INSTALLER_H
#define INSTALLER_H

void installer_init(void);
void installer_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void installer_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh);
void installer_open(void);

#endif
