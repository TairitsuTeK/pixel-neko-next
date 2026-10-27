#include "usb_hub.h"
#include "usb_core.h"
#include "log.h"
#include "timer.h"

/* USB Hub class constants (USB 2.0, chapter 11). */
#define USB_DT_HUB                 0x29

#define HUB_PORT_CONNECTION        0x0001
#define HUB_PORT_ENABLE            0x0002
#define HUB_PORT_SUSPEND           0x0004
#define HUB_PORT_OVER_CURRENT      0x0008
#define HUB_PORT_RESET             0x0010
#define HUB_PORT_POWER             0x0100
#define HUB_PORT_LOW_SPEED         0x0200
#define HUB_PORT_HIGH_SPEED        0x0400

#define HUB_C_PORT_CONNECTION      0x0001
#define HUB_C_PORT_ENABLE          0x0002
#define HUB_C_PORT_SUSPEND         0x0004
#define HUB_C_PORT_OVER_CURRENT    0x0008
#define HUB_C_PORT_RESET           0x0010

#define HUB_FEATURE_PORT_RESET     4
#define HUB_FEATURE_PORT_POWER     8
#define HUB_FEATURE_C_CONNECTION   16
#define HUB_FEATURE_C_ENABLE       17
#define HUB_FEATURE_C_SUSPEND      18
#define HUB_FEATURE_C_OVER_CURRENT 19
#define HUB_FEATURE_C_RESET        20

#define HUB_MAX_PORTS              8
#define HUB_MAX_DEPTH              3

static int hub_control(usb_device_t* hub, unsigned char request_type,
                       unsigned char request, unsigned short value,
                       unsigned short index, void* data, int len)
{
    if (!hub || !hub->hci || !hub->hci->control) return -1;

    usb_setup_packet_t setup;
    setup.bmRequestType = request_type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = (unsigned short)len;

    int dir = (request_type & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
    return hub->hci->control(hub->hci, hub, &setup, data, len, dir);
}

static int hub_get_status(usb_device_t* hub, int port,
                          unsigned short* status, unsigned short* change)
{
    unsigned char buf[4];
    usb_memset(buf, 0, sizeof(buf));

    unsigned char type = 0xA3;   /* IN, class, other (port) */
    unsigned short index = (unsigned short)port;
    if (port == 0) {
        type = 0xA0;             /* IN, class, device (hub itself) */
        index = 0;
    }

    if (hub_control(hub, type, USB_REQ_GET_STATUS, 0, index, buf, 4) < 0) {
        return -1;
    }
    if (status) *status = (unsigned short)(buf[0] | (buf[1] << 8));
    if (change) *change = (unsigned short)(buf[2] | (buf[3] << 8));
    return 0;
}

static int hub_set_feature(usb_device_t* hub, int port, int feature)
{
    return hub_control(hub, 0x23, USB_REQ_SET_FEATURE,
                       (unsigned short)feature, (unsigned short)port,
                       NULL, 0);
}

static int hub_clear_feature(usb_device_t* hub, int port, int feature)
{
    return hub_control(hub, 0x23, USB_REQ_CLEAR_FEATURE,
                       (unsigned short)feature, (unsigned short)port,
                       NULL, 0);
}

/* Acknowledge the hub's port-change latches. USB 2.0 hubs require these
 * features to be cleared individually; clearing them prevents the same
 * connection/reset event from being reported forever. */
static void hub_clear_changes(usb_device_t* hub, int port,
                              unsigned short change)
{
    if (change & HUB_C_PORT_CONNECTION)
        hub_clear_feature(hub, port, HUB_FEATURE_C_CONNECTION);
    if (change & HUB_C_PORT_ENABLE)
        hub_clear_feature(hub, port, HUB_FEATURE_C_ENABLE);
    if (change & HUB_C_PORT_SUSPEND)
        hub_clear_feature(hub, port, HUB_FEATURE_C_SUSPEND);
    if (change & HUB_C_PORT_OVER_CURRENT)
        hub_clear_feature(hub, port, HUB_FEATURE_C_OVER_CURRENT);
    if (change & HUB_C_PORT_RESET)
        hub_clear_feature(hub, port, HUB_FEATURE_C_RESET);
}
static void hub_port_reset(usb_device_t* hub, int port)
{
    unsigned short status = 0, change = 0;

    /* Clear any stale change bits before starting the reset. */
    if (hub_get_status(hub, port, &status, &change) == 0)
        hub_clear_changes(hub, port, change);

    if (hub_set_feature(hub, port, HUB_FEATURE_PORT_RESET) < 0) return;

    /* The hub drives reset for at least 10 ms; wait a little longer for
     * slow devices and hub firmware. */
    sleep_ms(50);
    for (int i = 0; i < 50; i++) {
        if (hub_get_status(hub, port, &status, &change) < 0) break;
        if (change & HUB_C_PORT_RESET) break;
        sleep_ms(10);
    }

    /* PORT_RESET is self-clearing; CLEAR_FEATURE(PORT_RESET) is not defined
     * by the hub class and some hubs (including QEMU's) stall it. */
    /* hub_clear_feature(hub, port, HUB_FEATURE_PORT_RESET); */
    sleep_ms(10);

    if (hub_get_status(hub, port, &status, &change) == 0)
        hub_clear_changes(hub, port, change);
}

int usb_hub_probe(usb_device_t* hub)
{
    if (!hub || !hub->hci || !hub->hci->supports_hubs) return -1;
    if (hub->hub_depth >= HUB_MAX_DEPTH) {
        log_printf("[HUB ] hub depth limit reached\n");
        return -1;
    }

    /* GET_DESCRIPTOR(HUB). The standard descriptor is 9 bytes; the
     * DeviceRemovable bitmap may make it longer, but bNbrPorts and
     * bPwrOn2PwrGood are within the first 9 bytes. */
    unsigned char desc[16];
    usb_memset(desc, 0, sizeof(desc));
    if (hub_control(hub, 0xA0, USB_REQ_GET_DESCRIPTOR,
                    (unsigned short)(USB_DT_HUB << 8), 0, desc, 9) < 0) {
        log_printf("[HUB ] descriptor read failed (addr %u)\n", hub->address);
        return -1;
    }
    if (desc[0] < 7 || desc[1] != USB_DT_HUB) {
        log_printf("[HUB ] invalid hub descriptor (addr %u)\n", hub->address);
        return -1;
    }

    int ports = desc[2];
    if (ports <= 0) {
        log_printf("[HUB ] no downstream ports\n");
        return 0;
    }
    if (ports > HUB_MAX_PORTS) {
        log_printf("[HUB ] %d ports, limiting to %d\n", ports, HUB_MAX_PORTS);
        ports = HUB_MAX_PORTS;
    }

    log_printf("[HUB ] addr %u: %d downstream port(s), characteristics=0x%X\n",
               hub->address, ports,
               desc[3] | (desc[4] << 8));

    /* Power all ports. A bus-powered/no-switching hub may reject the
     * request; that is harmless, so failures are intentionally ignored. */
    for (int port = 1; port <= ports; port++) {
        unsigned short status = 0, change = 0;
        if (hub_get_status(hub, port, &status, &change) < 0) continue;
        if (!(status & HUB_PORT_POWER))
            hub_set_feature(hub, port, HUB_FEATURE_PORT_POWER);
        hub_clear_changes(hub, port, change);
    }

    int power_delay = desc[5] * 2;
    if (power_delay < 50) power_delay = 50;
    if (power_delay > 500) power_delay = 500;
    sleep_ms(power_delay);

    int found = 0;
    for (int port = 1; port <= ports; port++) {
        unsigned short status = 0, change = 0;
        if (hub_get_status(hub, port, &status, &change) < 0) continue;

        if (!(status & HUB_PORT_CONNECTION)) {
            hub_clear_changes(hub, port, change);
            continue;
        }

        log_printf("[HUB ] addr %u port %d: device connected\n",
                   hub->address, port);
        hub_port_reset(hub, port);

        if (hub_get_status(hub, port, &status, &change) < 0) continue;
        hub_clear_changes(hub, port, change);

        if (!(status & HUB_PORT_ENABLE)) {
            log_printf("[HUB ] addr %u port %d: port not enabled after reset "
                       "(status=0x%X)\n", hub->address, port, status);
            continue;
        }

        int speed = USB_SPEED_FULL;
        if (status & HUB_PORT_LOW_SPEED) speed = USB_SPEED_LOW;
        else if (status & HUB_PORT_HIGH_SPEED) speed = USB_SPEED_HIGH;

        const char* speed_name = (speed == USB_SPEED_LOW) ? "low" :
                                 (speed == USB_SPEED_HIGH) ? "high" : "full";
        log_printf("[HUB ] addr %u port %d: %s-speed device, enumerating\n",
                   hub->address, port, speed_name);

        if (usb_enumerate_hub(hub->hci, hub, port, speed) == 0) found++;
    }

    log_printf("[HUB ] addr %u: %d device(s) enumerated\n", hub->address, found);
    return 0;
}

