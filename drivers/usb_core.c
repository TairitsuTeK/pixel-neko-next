#include "usb_core.h"
#include "usb_xhci.h"
#include "usb_ehci.h"
#include "usb_uhci.h"
#include "usb_hid.h"
#include "usb_hub.h"
#include "log.h"
#include "timer.h"

#define USB_MAX_DEVICES 8
#define USB_MAX_HCIS    4

/* UHCI exposes its legacy support register at a fixed PCI config offset.
 * Linux uses the same layout: bits outside the R/O and R/WC masks mean the
 * firmware has legacy PIRQ/SMI support armed. EHCI instead points at an
 * extended capability with BIOS/OS ownership semaphores. */
#define USB_LEGACY_UHCI_REG        0xC0
#define USB_LEGACY_UHCI_RWC        0x8F00
#define USB_LEGACY_UHCI_RO         0x5040
#define USB_LEGACY_EHCI_CAP_ID     0x01
#define USB_LEGACY_BIOS_OWNED      0x00010000U

static int g_usb_legacy_state = -1;

/* EHCI stores its extended capability list offset in HCCPARAMS. */
static int usb_ehci_find_legacy_capability(const pci_device_t* dev)
{
    unsigned int hccparams = pci_config_read(dev->bus, dev->slot,
                                             dev->function, 0x08);
    unsigned char eecp = (unsigned char)((hccparams >> 8) & 0xFFU);
    for (int i = 0; i < 32 && eecp >= 0x40; i++) {
        unsigned int cap = pci_config_read(dev->bus, dev->slot, dev->function,
                                           eecp);
        if ((cap & 0xFFU) == USB_LEGACY_EHCI_CAP_ID) return eecp;

        unsigned char next = (unsigned char)((cap >> 8) & 0xFFU);
        if (next == 0 || next == eecp) break;
        eecp = next;
    }
    return -1;
}

int usb_firmware_legacy_active(void)
{
    if (g_usb_legacy_state >= 0) return g_usb_legacy_state;

    g_usb_legacy_state = 0;
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;

        if (dev->prog_if == 0x00) {              /* UHCI */
            unsigned int legsup = pci_config_read(dev->bus, dev->slot,
                                                  dev->function,
                                                  USB_LEGACY_UHCI_REG) &
                                  0xFFFFU;
            /* Bits outside the R/O and R/WC masks mean firmware has left its
             * legacy PIRQ/SMI configuration active. QEMU reports the R/WC
             * status bits as 0x8F00 even without a CSM, so only the remaining
             * bits are a reliable "BIOS still owns it" signal. */
            if (legsup & ~(USB_LEGACY_UHCI_RO | USB_LEGACY_UHCI_RWC)) {
                log_printf("[USB ] firmware legacy USB present on UHCI "
                           "(bus %u slot %u, legsup=0x%X)\n",
                           dev->bus, dev->slot, legsup);
                g_usb_legacy_state = 1;
                break;
            }
        } else if (dev->prog_if == 0x20) {       /* EHCI */
            int cap = usb_ehci_find_legacy_capability(dev);
            if (cap >= 0) {
                unsigned int support = pci_config_read(dev->bus, dev->slot,
                                                       dev->function,
                                                       (unsigned char)cap);
                if (support & USB_LEGACY_BIOS_OWNED) {
                    log_printf("[USB ] firmware still owns EHCI legacy USB "
                               "(bus %u slot %u)\n", dev->bus, dev->slot);
                    g_usb_legacy_state = 1;
                    break;
                }
            }
        }
    }
    return g_usb_legacy_state;
}

static usb_device_t g_devices[USB_MAX_DEVICES];
static int g_device_count = 0;          /* number of used slots */
static unsigned char g_next_address = 1;

static usb_hci_t* g_hcis[USB_MAX_HCIS];
static int g_hci_count = 0;

void usb_memset(void* dst, unsigned char c, unsigned int n)
{
    unsigned char* p = (unsigned char*)dst;
    for (unsigned int i = 0; i < n; i++) p[i] = c;
}

void usb_memcpy(void* dst, const void* src, unsigned int n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
}

void usb_build_setup(usb_setup_packet_t* s, int dir, unsigned char request,
                     unsigned short value, unsigned short index,
                     unsigned short length)
{
    /* FIX: only USB_DIR_IN / USB_DIR_OUT are legal here; anything else would
     * silently produce a malformed bmRequestType. */
    if (dir != USB_DIR_IN && dir != USB_DIR_OUT) dir = USB_DIR_OUT;
    s->bmRequestType = (unsigned char)(dir | USB_RT_STANDARD | USB_RT_DEVICE);
    s->bRequest = request;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = length;
}

int usb_control_msg(usb_device_t* dev, int dir, unsigned char request,
                    unsigned short value, unsigned short index,
                    void* data, int len)
{
    if (!dev || !dev->hci || !dev->hci->control) return -1;
    usb_setup_packet_t setup;
    usb_build_setup(&setup, dir, request, value, index, (unsigned short)len);
    return dev->hci->control(dev->hci, dev, &setup, data, len, dir);
}

void usb_register_hci(usb_hci_t* hci)
{
    if (!hci) return;
    if (g_hci_count >= USB_MAX_HCIS) return;
    g_hcis[g_hci_count++] = hci;
}

/* FIX: device slots now carry a `used` flag. Allocation picks the first free
 * slot, and release takes the concrete device pointer so a failed enumeration
 * in the middle of the array cannot leak or alias a slot. */
static usb_device_t* usb_alloc_device(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            usb_memset(&g_devices[i], 0, sizeof(g_devices[i]));
            g_devices[i].used = 1;
            g_device_count++;
            return &g_devices[i];
        }
    }
    return NULL;
}

static void usb_release_device(usb_device_t* dev)
{
    if (!dev) return;
    if (dev < &g_devices[0] || dev >= &g_devices[USB_MAX_DEVICES]) return;
    if (!dev->used) return;
    dev->used = 0;
    if (g_device_count > 0) g_device_count--;
}

/* Parse a configuration descriptor blob, filling in the endpoint list.
 * Returns the interface number of the first HID interface, or -1 when none
 * is present. FIX: returning the interface number (instead of a bool) lets
 * the caller address the correct interface on composite devices. */
static int usb_parse_config(usb_device_t* dev, const unsigned char* cfg,
                            int total_len)
{
    dev->endpoint_count = 0;
    int hid_iface = -1;
    int off = 0;

    while (off + 2 <= total_len) {
        unsigned char len = cfg[off];
        unsigned char type = cfg[off + 1];
        if (len < 2) break;

        if (type == USB_DT_INTERFACE && len >= 9) {
            const usb_interface_desc_t* ifd =
                (const usb_interface_desc_t*)(cfg + off);
            if (ifd->bInterfaceClass == 0x03 && hid_iface < 0) {
                hid_iface = ifd->bInterfaceNumber;
                dev->interface_number = ifd->bInterfaceNumber;
                dev->interface_protocol = ifd->bInterfaceProtocol;
            }
        } else if (type == USB_DT_ENDPOINT && len >= 7) {
            if (dev->endpoint_count < USB_MAX_ENDPOINTS) {
                const usb_endpoint_desc_t* epd =
                    (const usb_endpoint_desc_t*)(cfg + off);
                usb_endpoint_t* ep = &dev->endpoints[dev->endpoint_count++];
                ep->address = epd->bEndpointAddress;
                ep->direction = (epd->bEndpointAddress & 0x80) ?
                    USB_DIR_IN : USB_DIR_OUT;
                ep->type = epd->bmAttributes & 0x03;
                ep->max_packet = epd->wMaxPacketSize;
                ep->interval = epd->bInterval;
                ep->toggle = 0;
            }
        }
        off += len;
    }
    return hid_iface;
}

/* Pick an address in 1..127, never 0, and never let the counter run past
 * 127 before wrapping. */
static unsigned char usb_next_address(void)
{
    if (g_next_address == 0 || g_next_address > 127) g_next_address = 1;
    unsigned char addr = g_next_address++;
    if (g_next_address > 127) g_next_address = 1;
    return addr;
}

static int usb_enumerate_device(usb_hci_t* hci, int port, int speed,
                                unsigned char parent_hub,
                                unsigned char parent_port,
                                unsigned char hub_depth)
{
    usb_device_t* dev = usb_alloc_device();
    if (!dev) {
        log_printf("[USB ] port %d: no free device slot\n", port);
        return -1;
    }

    dev->hci = hci;
    dev->port = (unsigned char)port;
    dev->parent_hub = parent_hub;
    dev->parent_port = parent_port;
    dev->hub_depth = hub_depth;
    dev->speed = (unsigned char)speed;
    dev->address = 0;
    /* High-speed EP0 has a 64-byte max packet; full/low-speed uses 8 for the
     * first descriptor request. */
    dev->max_packet0 = (speed == USB_SPEED_HIGH) ? 64 : 8;

    /* Read the first 8 bytes of the device descriptor at address 0. */
    usb_device_desc_t ddesc;
    usb_memset(&ddesc, 0, sizeof(ddesc));
    int rc = usb_control_msg(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (unsigned short)(USB_DT_DEVICE << 8), 0,
                             &ddesc, 8);
    if (rc < 0) {
        log_printf("[USB ] port %d: no device descriptor\n", port);
        usb_release_device(dev);
        return -1;
    }

    /* FIX: validate the descriptor header before trusting bMaxPacketSize0. */
    if (ddesc.bLength < 8 || ddesc.bDescriptorType != USB_DT_DEVICE) {
        log_printf("[USB ] port %d: invalid device descriptor header "
                   "(len=%u type=%u)\n", port, ddesc.bLength,
                   ddesc.bDescriptorType);
        usb_release_device(dev);
        return -1;
    }
    if (ddesc.bMaxPacketSize0 > 0) dev->max_packet0 = ddesc.bMaxPacketSize0;

    /* Assign a unique address. The status stage of SET_ADDRESS completes on
     * the old address (0), so only update dev->address afterwards. */
    unsigned char new_addr = usb_next_address();
    if (usb_control_msg(dev, USB_DIR_OUT, USB_REQ_SET_ADDRESS, new_addr, 0,
                        NULL, 0) < 0) {
        log_printf("[USB ] port %d: SET_ADDRESS %u failed\n", port, new_addr);
        usb_release_device(dev);
        return -1;
    }
    sleep_ms(2);
    dev->address = new_addr;

    /* Read the full device descriptor using the new address. */
    usb_memset(&ddesc, 0, sizeof(ddesc));
    if (usb_control_msg(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                        (unsigned short)(USB_DT_DEVICE << 8), 0,
                        &ddesc, sizeof(ddesc)) < 0) {
        log_printf("[USB ] port %d: full device descriptor failed\n", port);
        usb_release_device(dev);
        return -1;
    }
    if (ddesc.bLength < 8 || ddesc.bDescriptorType != USB_DT_DEVICE) {
        log_printf("[USB ] port %d: invalid full device descriptor\n", port);
        usb_release_device(dev);
        return -1;
    }
    dev->vendor = ddesc.idVendor;
    dev->product = ddesc.idProduct;
    dev->class_code = ddesc.bDeviceClass;
    dev->subclass = ddesc.bDeviceSubClass;
    dev->protocol = ddesc.bDeviceProtocol;

    /* Read the configuration descriptor (first 9 bytes for its length). */
    usb_config_desc_t cdesc;
    usb_memset(&cdesc, 0, sizeof(cdesc));
    if (usb_control_msg(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                        (unsigned short)(USB_DT_CONFIG << 8), 0,
                        &cdesc, 9) < 0) {
        log_printf("[USB ] vid=%X pid=%X (no configuration)\n",
                   dev->vendor, dev->product);
        /* A device without configuration is still valid in the addressed
         * state; keep it so the caller can inspect it. */
        return 0;
    }
    if (cdesc.bLength < 9 || cdesc.bDescriptorType != USB_DT_CONFIG) {
        log_printf("[USB ] vid=%X pid=%X: invalid config descriptor\n",
                   dev->vendor, dev->product);
        usb_release_device(dev);
        return -1;
    }

    /* FIX: use a larger buffer and honour the real wTotalLength; log when the
     * descriptor is truncated instead of silently dropping endpoints. */
    unsigned char cfg[1024];
    int total = cdesc.wTotalLength;
    if (total <= 0) {
        total = (int)sizeof(cfg);
    } else if (total > (int)sizeof(cfg)) {
        log_printf("[USB ] vid=%X pid=%X: config %d bytes, truncating to %d\n",
                   dev->vendor, dev->product, total, (int)sizeof(cfg));
        total = (int)sizeof(cfg);
    }
    usb_memset(cfg, 0, sizeof(cfg));
    rc = usb_control_msg(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                         (unsigned short)(USB_DT_CONFIG << 8), 0, cfg, total);
    if (rc < 0) {
        log_printf("[USB ] vid=%X pid=%X: config read failed\n",
                   dev->vendor, dev->product);
        usb_release_device(dev);
        return -1;
    }

    int hid_iface = usb_parse_config(dev, cfg, total);

    /* FIX: fail the whole enumeration when SET_CONFIGURATION is rejected;
     * endpoints are unusable in the addressed state. */
    if (usb_control_msg(dev, USB_DIR_OUT, USB_REQ_SET_CONFIGURATION,
                        cdesc.bConfigurationValue, 0, NULL, 0) < 0) {
        log_printf("[USB ] vid=%X pid=%X: SET_CONFIGURATION failed\n",
                   dev->vendor, dev->product);
        usb_release_device(dev);
        return -1;
    }
    dev->config_value = cdesc.bConfigurationValue;

    const char* speed_name = (dev->speed == USB_SPEED_LOW) ? "low" :
                             (dev->speed == USB_SPEED_FULL) ? "full" : "high";
    log_printf("[USB ] vid=%X pid=%X class=%X speed=%s ep=%d\n",
               dev->vendor, dev->product, dev->class_code, speed_name,
               dev->endpoint_count);

    /* FIX: HID attach failures must release the slot; otherwise repeated
     * hotplug leaks every slot that ever had a broken HID descriptor. */
    if (dev->class_code == 0x09) {
        if (hci->supports_hubs) {
            usb_hub_probe(dev);
        } else {
            log_printf("[USB ] vid=%X pid=%X: hub enumeration unsupported "
                       "on %s\n", dev->vendor, dev->product, hci->name);
        }
    }

    if (hid_iface >= 0 || dev->class_code == 0x03) {
        if (usb_hid_attach(dev) != 0) {
            log_printf("[USB ] vid=%X pid=%X: HID attach failed\n",
                       dev->vendor, dev->product);
            usb_release_device(dev);
            return -1;
        }
    }

    return 0;
}

int usb_enumerate(usb_hci_t* hci, int port, int speed)
{
    return usb_enumerate_device(hci, port, speed, 0, 0, 0);
}

int usb_enumerate_hub(usb_hci_t* hci, usb_device_t* hub, int port, int speed)
{
    if (!hub || hub->hub_depth >= 3) return -1;
    return usb_enumerate_device(hci, port, speed, hub->address,
                                (unsigned char)port,
                                (unsigned char)(hub->hub_depth + 1));
}

void usb_init(void)
{
    /* A CSM may still own UHCI/EHCI legacy support and generate PS/2 traffic
     * through SMM. Each controller probe checks its own ownership state; no
     * global reset is performed here because resetting a controller behind the
     * firmware's back is what deadlocks some H55/H57 boards at "Starting
     * desktop". The probes either take over a clean controller or leave the
     * firmware's PS/2 emulation intact. */
    log_printf("[USB ] scanning PCI for USB host controllers\n");

    /* xHCI first: since Skylake the PCH has no EHCI/UHCI left, so every USB
     * 2.0 port of a modern board is owned by the xHCI controller. */
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x30) {
            if (xhci_probe(dev) != 0) {
                log_printf("[USB ] xHCI probe failed (bus %u slot %u)\n",
                           dev->bus, dev->slot);
            }
        }
    }

    /* Bring up EHCI first. Full/low-speed ports are handed back to the
     * companion controllers during EHCI probe, so UHCI must scan afterwards. */
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x20) {
            if (ehci_probe(dev) != 0) {
                log_printf("[USB ] EHCI probe failed (bus %u slot %u)\n",
                           dev->bus, dev->slot);
            }
        }
    }

    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x00) {
            if (uhci_probe(dev) != 0) {
                log_printf("[USB ] UHCI probe failed (bus %u slot %u)\n",
                           dev->bus, dev->slot);
            }
        }
    }

    log_printf("[USB ] %d controller(s), %d device(s) enumerated\n",
               g_hci_count, g_device_count);
}

void usb_poll(void)
{
    for (int i = 0; i < g_hci_count; i++) {
        if (g_hcis[i] && g_hcis[i]->poll) {
            g_hcis[i]->poll(g_hcis[i]);
        }
    }
    usb_hid_poll();
}