#ifndef USB_XHCI_H
#define USB_XHCI_H

/*
 * usb_xhci.h - eXtensible Host Controller Interface (xHCI) driver.
 *
 * xHCI is the USB 3.x host controller, but on every Intel platform since
 * Skylake it also owns *all* USB 2.0 ports (there is no EHCI/UHCI left in the
 * PCH). Driving xHCI is therefore the only way to get a USB 2.0 keyboard or
 * mouse working natively on a modern board.
 *
 * This driver implements the USB 2.0 half only: low/full/high-speed devices,
 * the EP0 control ring and one interrupt IN endpoint per HID device (boot
 * protocol). USB 3.x SuperSpeed ports are not used.
 */

#include "usb_core.h"

int  xhci_probe(const pci_device_t* dev);
void xhci_poll(void);

#endif /* USB_XHCI_H */
