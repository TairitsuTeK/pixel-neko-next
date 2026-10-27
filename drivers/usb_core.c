#include "usb_core.h"
#include "usb_xhci.h"
#include "usb_ehci.h"
#include "usb_uhci.h"
#include "usb_hid.h"
#include "log.h"
#include "timer.h"

#define USB_MAX_DEVICES 8
#define USB_MAX_HCIS    4

static usb_device_t g_devices[USB_MAX_DEVICES];
static int g_device_count = 0;
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

static usb_device_t* usb_alloc_device(void)
{
    if (g_device_count >= USB_MAX_DEVICES) return NULL;
    usb_device_t* dev = &g_devices[g_device_count];
    usb_memset(dev, 0, sizeof(*dev));
    g_device_count++;
    return dev;
}

static void usb_release_device(void)
{
    if (g_device_count > 0) g_device_count--;
}

/* Parse a configuration descriptor blob, filling in the endpoint list.
 * Returns 1 when an HID interface is present, 0 otherwise. */
static int usb_parse_config(usb_device_t* dev, const unsigned char* cfg,
                            int total_len)
{
    dev->endpoint_count = 0;
    int hid = 0;
    int off = 0;

    while (off + 2 <= total_len) {
        unsigned char len = cfg[off];
        unsigned char type = cfg[off + 1];
        if (len < 2) break;

        if (type == USB_DT_INTERFACE && len >= 9) {
            const usb_interface_desc_t* ifd =
                (const usb_interface_desc_t*)(cfg + off);
            if (ifd->bInterfaceClass == 0x03) {
                hid = 1;
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
    return hid;
}

int usb_enumerate(usb_hci_t* hci, int port, int speed)
{
    usb_device_t* dev = usb_alloc_device();
    if (!dev) return -1;

    dev->hci = hci;
    dev->port = (unsigned char)port;
    dev->speed = (unsigned char)speed;
    dev->address = 0;
    /* High-speed EP0 has a 64-byte max packet; full/low-speed uses 8 for the
     * first descriptor request. */
    dev->max_packet0 = (speed == USB_SPEED_HIGH) ? 64 : 8;

    /* Read the first 8 bytes of the device descriptor at address 0. */
    usb_device_desc_t ddesc;
    usb_memset(&ddesc, 0, sizeof(ddesc));
    if (usb_control_msg(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                        (unsigned short)(USB_DT_DEVICE << 8), 0,
                        &ddesc, 8) < 0) {
        log_printf("[USB ] port %d: no device descriptor\n", port);
        usb_release_device();
        return -1;
    }
    if (ddesc.bMaxPacketSize0 > 0) dev->max_packet0 = ddesc.bMaxPacketSize0;

    /* Assign a unique address. The status stage of SET_ADDRESS completes on
     * the old address (0), so only update dev->address afterwards. */
    unsigned char new_addr = g_next_address++;
    if (g_next_address > 127) g_next_address = 1;
    if (usb_control_msg(dev, USB_DIR_OUT, USB_REQ_SET_ADDRESS, new_addr, 0,
                        NULL, 0) < 0) {
        log_printf("[USB ] port %d: SET_ADDRESS failed\n", port);
        usb_release_device();
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
        usb_release_device();
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
        return 0;
    }

    unsigned char cfg[256];
    int total = cdesc.wTotalLength;
    if (total <= 0 || total > (int)sizeof(cfg)) total = (int)sizeof(cfg);
    usb_memset(cfg, 0, sizeof(cfg));
    usb_control_msg(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                    (unsigned short)(USB_DT_CONFIG << 8), 0, cfg, total);

    int hid = usb_parse_config(dev, cfg, total);

    usb_control_msg(dev, USB_DIR_OUT, USB_REQ_SET_CONFIGURATION,
                    cdesc.bConfigurationValue, 0, NULL, 0);
    dev->config_value = cdesc.bConfigurationValue;

    const char* speed_name = (dev->speed == USB_SPEED_LOW) ? "low" :
                             (dev->speed == USB_SPEED_FULL) ? "full" : "high";
    log_printf("[USB ] vid=%X pid=%X class=%X speed=%s ep=%d\n",
               dev->vendor, dev->product, dev->class_code, speed_name,
               dev->endpoint_count);

    if (hid || dev->class_code == 0x03) {
        usb_hid_attach(dev);
    }

    return 0;
}

void usb_init(void)
{
    log_printf("[USB ] scanning PCI for USB host controllers\n");

    /* xHCI first: since Skylake the PCH has no EHCI/UHCI left, so every USB
     * 2.0 port of a modern board is owned by the xHCI controller. */
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x30) xhci_probe(dev);
    }

    /* Bring up EHCI first. Full/low-speed ports are handed back to the
     * companion controllers during EHCI probe, so UHCI must scan afterwards. */
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x20) ehci_probe(dev);
    }

    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x00) uhci_probe(dev);
    }

    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) continue;
        if (dev->prog_if == 0x10) {
            log_printf("[USB ] OHCI (bus %u slot %u) not supported\n",
                       dev->bus, dev->slot);
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

