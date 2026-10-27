#include "usb_hid.h"
#include "usb_core.h"
#include "keyboard.h"
#include "mouse.h"
#include "irq.h"
#include "log.h"

#define USB_HID_MAX_DEVICES 4

typedef struct {
    usb_device_t*   dev;
    usb_endpoint_t* ep_in;
    int is_keyboard;
    int is_mouse;
    unsigned char prev_mods;
    unsigned char prev_keys[6];
} usb_hid_dev_t;

static usb_hid_dev_t g_hids[USB_HID_MAX_DEVICES];
static int g_hid_count = 0;
static int g_usb_keyboard_count = 0;
static int g_usb_mouse_count = 0;

/* Translate a HID keyboard usage code to PS/2 scancode set 1.
 * Sets *e0 to 1 when an E0 prefix must be emitted first. */
static unsigned char hid_usage_to_scancode(unsigned char usage, int* e0)
{
    *e0 = 0;

    static const unsigned char alpha[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
        0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
    };

    if (usage >= 0x04 && usage <= 0x1D) return alpha[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x27) return (unsigned char)(usage - 0x1E + 0x02);

    switch (usage) {
        case 0x28: return 0x1C;  /* Enter */
        case 0x29: return 0x01;  /* Esc */
        case 0x2A: return 0x0E;  /* Backspace */
        case 0x2B: return 0x0F;  /* Tab */
        case 0x2C: return 0x39;  /* Space */
        case 0x2D: return 0x0C;  /* - */
        case 0x2E: return 0x0D;  /* = */
        case 0x2F: return 0x1A;  /* [ */
        case 0x30: return 0x1B;  /* ] */
        case 0x31: return 0x2B;  /* backslash */
        case 0x33: return 0x27;  /* ; */
        case 0x34: return 0x28;  /* ' */
        case 0x35: return 0x29;  /* ` */
        case 0x36: return 0x33;  /* , */
        case 0x37: return 0x34;  /* . */
        case 0x38: return 0x35;  /* / */
        case 0x39: return 0x3A;  /* CapsLock */
        case 0x3A: return 0x3B;  /* F1 */
        case 0x3B: return 0x3C;  /* F2 */
        case 0x3C: return 0x3D;  /* F3 */
        case 0x3D: return 0x3E;  /* F4 */
        case 0x3E: return 0x3F;  /* F5 */
        case 0x3F: return 0x40;  /* F6 */
        case 0x40: return 0x41;  /* F7 */
        case 0x41: return 0x42;  /* F8 */
        case 0x42: return 0x43;  /* F9 */
        case 0x43: return 0x44;  /* F10 */
        case 0x44: return 0x57;  /* F11 */
        case 0x45: return 0x58;  /* F12 */
        case 0x49: *e0 = 1; return 0x52;  /* Insert */
        case 0x4A: *e0 = 1; return 0x47;  /* Home */
        case 0x4B: *e0 = 1; return 0x49;  /* PageUp */
        case 0x4C: *e0 = 1; return 0x53;  /* Delete */
        case 0x4D: *e0 = 1; return 0x4F;  /* End */
        case 0x4E: *e0 = 1; return 0x51;  /* PageDown */
        case 0x4F: *e0 = 1; return 0x4D;  /* RightArrow */
        case 0x50: *e0 = 1; return 0x4B;  /* LeftArrow */
        case 0x51: *e0 = 1; return 0x50;  /* DownArrow */
        case 0x52: *e0 = 1; return 0x48;  /* UpArrow */
        default: return 0;
    }
}

static void hid_send_key(unsigned char usage, int release)
{
    if (usage == 0 || usage >= 0xE0) return;

    int e0 = 0;
    unsigned char sc = hid_usage_to_scancode(usage, &e0);
    if (sc == 0) return;
    if (release) sc |= 0x80;

    if (e0) keyboard_handle_usb_scancode(0xE0);
    keyboard_handle_usb_scancode(sc);
}

static void hid_process_keyboard(usb_hid_dev_t* h, const unsigned char* report)
{
    unsigned char mods = report[0];
    unsigned char keys[6];
    for (int i = 0; i < 6; i++) keys[i] = report[2 + i];

    unsigned char changed = mods ^ h->prev_mods;

    if (changed & 0x01) /* LeftCtrl */
        keyboard_handle_byte((mods & 0x01) ? 0x1D : (0x1D | 0x80));
    if (changed & 0x02) /* LeftShift */
        keyboard_handle_byte((mods & 0x02) ? 0x2A : (0x2A | 0x80));
    if (changed & 0x04) /* LeftAlt */
        keyboard_handle_byte((mods & 0x04) ? 0x38 : (0x38 | 0x80));
    if (changed & 0x10) { /* RightCtrl */
        keyboard_handle_byte(0xE0);
        keyboard_handle_byte((mods & 0x10) ? 0x1D : (0x1D | 0x80));
    }
    if (changed & 0x20) /* RightShift */
        keyboard_handle_byte((mods & 0x20) ? 0x36 : (0x36 | 0x80));
    if (changed & 0x40) { /* RightAlt */
        keyboard_handle_byte(0xE0);
        keyboard_handle_byte((mods & 0x40) ? 0x38 : (0x38 | 0x80));
    }
    h->prev_mods = mods;

    /* Newly pressed keys. */
    for (int i = 0; i < 6; i++) {
        if (keys[i] == 0) continue;
        int found = 0;
        for (int j = 0; j < 6; j++) {
            if (h->prev_keys[j] == keys[i]) { found = 1; break; }
        }
        if (!found) hid_send_key(keys[i], 0);
    }

    /* Released keys. */
    for (int j = 0; j < 6; j++) {
        if (h->prev_keys[j] == 0) continue;
        int found = 0;
        for (int i = 0; i < 6; i++) {
            if (keys[i] == h->prev_keys[j]) { found = 1; break; }
        }
        if (!found) hid_send_key(h->prev_keys[j], 1);
    }

    for (int i = 0; i < 6; i++) h->prev_keys[i] = keys[i];
}

static void hid_process_mouse(usb_hid_dev_t* h, const unsigned char* report)
{
    (void)h;
    mouse_handle_usb_report((unsigned char)(report[0] & 0x07),
                            (signed char)report[1],
                            (signed char)report[2]);
}

int usb_hid_attach(usb_device_t* dev)
{
    if (g_hid_count >= USB_HID_MAX_DEVICES) return -1;

    if (!dev->hci || !dev->hci->intr_in) {
        log_printf("[HID ] %s controller has no interrupt IN support\n",
                   dev->hci ? dev->hci->name : "unknown");
        return -1;
    }

    usb_endpoint_t* ep_in = 0;
    for (int i = 0; i < dev->endpoint_count; i++) {
        usb_endpoint_t* ep = &dev->endpoints[i];
        if (ep->type == USB_EP_TYPE_INTERRUPT &&
            ep->direction == USB_DIR_IN) {
            ep_in = ep;
            break;
        }
    }
    if (!ep_in) {
        log_printf("[HID ] no interrupt IN endpoint\n");
        return -1;
    }

    int is_kbd = (dev->interface_protocol == 1);
    int is_mouse = (dev->interface_protocol == 2);
    if (!is_kbd && !is_mouse) {
        log_printf("[HID ] unknown protocol %u, assuming keyboard\n",
                   dev->interface_protocol);
        is_kbd = 1;
    }

    /* Request HID boot protocol. */
    usb_setup_packet_t setup;
    usb_memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x21;  /* Host-to-device, Class, Interface */
    setup.bRequest = 0x0B;       /* SET_PROTOCOL */
    setup.wValue = 0;            /* boot protocol */
    setup.wIndex = dev->interface_number;
    setup.wLength = 0;
    dev->hci->control(dev->hci, dev, &setup, NULL, 0, USB_DIR_OUT);

    usb_hid_dev_t* h = &g_hids[g_hid_count];
    usb_memset(h, 0, sizeof(*h));
    h->dev = dev;
    h->ep_in = ep_in;
    h->is_keyboard = is_kbd;
    h->is_mouse = is_mouse;
    if (is_kbd) g_usb_keyboard_count++;
    else if (is_mouse) g_usb_mouse_count++;
    g_hid_count++;

    log_printf("[HID ] %s attached (ep 0x%X)\n",
               is_kbd ? "keyboard" : "mouse", ep_in->address);
    return 0;
}

int usb_hid_keyboard_count(void)
{
    return g_usb_keyboard_count;
}

int usb_hid_mouse_count(void)
{
    return g_usb_mouse_count;
}

void usb_hid_poll(void)
{
    for (int i = 0; i < g_hid_count; i++) {
        usb_hid_dev_t* h = &g_hids[i];
        if (!h->dev || !h->ep_in || !h->dev->hci || !h->dev->hci->intr_in) {
            continue;
        }

        unsigned char report[8];
        usb_memset(report, 0, sizeof(report));
        int len = h->dev->hci->intr_in(h->dev->hci, h->dev, h->ep_in,
                                       report, sizeof(report));
        if (len > 0) {
            /* Keep one HID report atomic with respect to PS/2 IRQs. Otherwise
             * IRQ1/IRQ12 can inject a byte between the individual key/scancode
             * updates and corrupt the shared keyboard state. */
            unsigned int flags = irq_save_flags();
            if (h->is_keyboard) hid_process_keyboard(h, report);
            else if (h->is_mouse) hid_process_mouse(h, report);
            irq_restore_flags(flags);
        }
    }
}

