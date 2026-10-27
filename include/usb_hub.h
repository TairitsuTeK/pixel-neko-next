#ifndef USB_HUB_H
#define USB_HUB_H

#include "usb_core.h"

/*
 * usb_hub.h - USB Hub class driver for external hubs and Intel Integrated
 * Rate Matching Hubs (RMH) found behind EHCI root ports on 5 Series PCH.
 */

/* Enumerate and configure a class-9 hub, then probe all connected downstream
 * ports. Returns 0 when the hub was handled, negative on failure. */
int usb_hub_probe(usb_device_t* hub);

#endif /* USB_HUB_H */
