#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_handle_byte(unsigned char scancode);
int keyboard_get_key(void);
int keyboard_get_ctrl(void);
void ps2_input_poll(void);

#endif
