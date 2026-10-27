#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_handle_byte(unsigned char scancode);
void keyboard_handle_usb_scancode(unsigned char scancode);
int keyboard_get_key(void);
int keyboard_get_ctrl(void);
int keyboard_is_ps2_active(void);
void ps2_input_poll(void);

#endif
