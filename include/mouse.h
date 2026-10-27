#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_handle_byte(unsigned char d);
void mouse_handle_usb_report(unsigned char buttons, int dx, int dy);
void mouse_get(int* x, int* y, int* btn);
void mouse_draw(unsigned char* vga);
void mouse_reset(unsigned char* vga);
void mouse_invalidate(void);

extern int mouse_speed;
void mouse_set_speed(int s);
int mouse_get_speed(void);

#endif