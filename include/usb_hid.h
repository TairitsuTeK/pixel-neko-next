#ifndef USB_HID_H
#define USB_HID_H

/*
 * usb_hid.h - USB Human Interface Device (HID) class driver.
 *
 * Implements the HID boot protocol for keyboards and mice and translates the
 * reports into the existing PS/2 keyboard / mouse input pipeline.
 */

#include "usb_core.h"

int  usb_hid_attach(usb_device_t* dev);
void usb_hid_poll(void);

#endif /* USB_HID_H */
