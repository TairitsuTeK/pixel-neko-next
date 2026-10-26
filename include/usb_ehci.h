#ifndef USB_EHCI_H
#define USB_EHCI_H

/*
 * usb_ehci.h - Enhanced Host Controller Interface (EHCI) driver.
 *
 * EHCI is the USB 2.0 high-speed (480 Mbps) host controller. It handles
 * high-speed devices directly and hands low/full-speed traffic to its
 * companion controllers (UHCI/OHCI).
 */

#include "usb_core.h"

int  ehci_probe(const pci_device_t* dev);
void ehci_poll(void);

#endif /* USB_EHCI_H */
