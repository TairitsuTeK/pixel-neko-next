#ifndef USB_UHCI_H
#define USB_UHCI_H

/*
 * usb_uhci.h - Universal Host Controller Interface (UHCI) driver.
 *
 * UHCI is a USB 1.1 full/low-speed (1.5/12 Mbps) host controller that maps
 * its registers into PCI I/O space. On many Intel/VIA chipsets UHCI acts as
 * the companion controller to EHCI and carries low/full-speed devices.
 */

#include "usb_core.h"

int  uhci_probe(const pci_device_t* dev);
void uhci_poll(void);

#endif /* USB_UHCI_H */
