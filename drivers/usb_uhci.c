#include "usb_uhci.h"
#include "usb_core.h"
#include "port.h"
#include "mm.h"
#include "log.h"
#include "timer.h"

/* ---- UHCI registers (I/O space, 16-bit, offset from BAR0) ---- */
#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRNUM     0x06
#define UHCI_FRBASEADD 0x08
#define UHCI_SOFMOD    0x0C
#define UHCI_PORTSC1   0x10
#define UHCI_PORTSC2   0x12

/* USBCMD bits */
#define UHCI_CMD_RS      0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004
#define UHCI_CMD_CF      0x0040
#define UHCI_CMD_MAXP    0x0080

/* USBSTS bits */
#define UHCI_STS_USBINT   0x0001
#define UHCI_STS_USBERR   0x0002
#define UHCI_STS_HCHALTED 0x0020

/* PORTSC bits (UHCI spec: PR is bit 9, LSDA is bit 8) */
#define UHCI_PORT_CCS  0x0001  /* current connect status */
#define UHCI_PORT_CSC  0x0002  /* connect status change */
#define UHCI_PORT_PE   0x0004  /* port enable */
#define UHCI_PORT_PEC  0x0008  /* port enable change */
#define UHCI_PORT_RD   0x0040  /* resume detect */
#define UHCI_PORT_LSDA 0x0100  /* low speed device attached */
#define UHCI_PORT_PR   0x0200  /* port reset */
#define UHCI_PORT_SUSP 0x1000  /* suspend */
#define UHCI_PORT_WC   (UHCI_PORT_CSC | UHCI_PORT_PEC)

/* ---- UHCI legacy support (PCI config, fixed offset) ---- */
#define UHCI_LEGACY_SUP 0xC0
#define UHCI_LEGACY_RWC 0x8F00
#define UHCI_LEGACY_RO  0x5040

/* ---- USB packet IDs (token) ---- */
#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1

/* ---- TD control/status bits ---- */
#define UHCI_TD_ACTIVE   0x00800000
#define UHCI_TD_IOC      0x01000000
#define UHCI_TD_ISO      0x02000000
#define UHCI_TD_LS       0x04000000
#define UHCI_TD_CERR     0x18000000  /* 2-bit error counter = 3 */
#define UHCI_TD_SPD      0x20000000
#define UHCI_TD_STALLED  0x00400000
#define UHCI_TD_DBUFERR  0x00200000
#define UHCI_TD_BABBLE   0x00100000
#define UHCI_TD_NAK      0x00080000
#define UHCI_TD_CRC      0x00040000
#define UHCI_TD_BITSTUFF 0x00020000
#define UHCI_TD_ERROR_MASK (UHCI_TD_STALLED | UHCI_TD_DBUFERR | \
                            UHCI_TD_BABBLE | UHCI_TD_NAK | UHCI_TD_CRC | \
                            UHCI_TD_BITSTUFF)

/* The 11-bit MaxLen field in a UHCI token stores (length - 1); 0x7FF means a
 * zero-length packet. The controller adds one back before using the value. */
static unsigned int uhci_len_field(int len)
{
    if (len <= 0) return 0x7FF;
    if (len > 0x7FE) len = 0x7FE;
    return (unsigned int)(len - 1);
}

/* ---- Transfer Descriptor (32 bytes, 16-byte aligned) ---- */
typedef struct {
    volatile unsigned int link;
    volatile unsigned int status;
    volatile unsigned int token;
    volatile unsigned int buffer;
} __attribute__((aligned(16))) uhci_td_t;

/* ---- Queue Head (32 bytes, 16-byte aligned) ---- */
typedef struct {
    volatile unsigned int head;
    volatile unsigned int element;
    unsigned int reserved[6];
} __attribute__((aligned(16))) uhci_qh_t;

#define UHCI_TD_POOL 32
#define UHCI_MAX_PORTS 2
#define UHCI_MAX_INTR_TDS 4
#define UHCI_MAX_HID_QH 4

typedef struct {
    int used;
    int persistent;   /* 1 = keep polling (HID interrupt endpoint) */
    int ep_addr;
    int dev_addr;
    int max_len;
    unsigned char* buffer;
} uhci_td_owner_t;

typedef struct {
    unsigned short io_base;
    unsigned int frame_list_phys;
    unsigned int* frame_list;

    uhci_td_t tds[UHCI_TD_POOL];
    uhci_td_owner_t owners[UHCI_TD_POOL];

    /* One queue head per HID endpoint keeps each interrupt TD scheduled even
     * after the controller advances the QH element pointer. */
    uhci_qh_t intr_qh[UHCI_MAX_HID_QH];
    unsigned char intr_qh_used[UHCI_MAX_HID_QH];
    unsigned char intr_qh_dev[UHCI_MAX_HID_QH];
    unsigned char intr_qh_ep[UHCI_MAX_HID_QH];
    /* Persistent interrupt report buffers: the controller must never DMA into
     * the caller's stack buffer after usb_hid_poll() returns. */
    unsigned char intr_buf[UHCI_MAX_HID_QH][64];

    uhci_qh_t control_qh;

    usb_hci_t hci;
} uhci_ctrl_t;

static uhci_ctrl_t g_uhci[4];
static int g_uhci_count = 0;

static unsigned short uhci_read(uhci_ctrl_t* c, unsigned short reg)
{
    return inw(c->io_base + reg);
}

static void uhci_write(uhci_ctrl_t* c, unsigned short reg, unsigned short val)
{
    outw(c->io_base + reg, val);
}

/* Encode a frame-list / link pointer value. */
static unsigned int uhci_link_qh(unsigned int phys)
{
    return (phys & ~0xFU) | 0x2U;   /* Q=1, T=0 */
}

static unsigned int uhci_link_td(unsigned int phys)
{
    return (phys & ~0xFU) | 0x0U;   /* Q=0, T=0 */
}

static unsigned int uhci_link_terminate(void)
{
    return 0x1U;                    /* T=1 */
}

static unsigned int td_phys(uhci_ctrl_t* c, int index)
{
    return (unsigned int)&c->tds[index];
}

static void td_init(uhci_ctrl_t* c, int index)
{
    c->tds[index].link = uhci_link_terminate();
    c->tds[index].status = 0;
    c->tds[index].token = 0;
    c->tds[index].buffer = 0;
    c->owners[index].used = 0;
    c->owners[index].persistent = 0;
    c->owners[index].ep_addr = 0;
    c->owners[index].dev_addr = 0;
    c->owners[index].max_len = 0;
    c->owners[index].buffer = 0;
}

static int td_alloc(uhci_ctrl_t* c)
{
    for (int i = 0; i < UHCI_TD_POOL; i++) {
        if (!c->owners[i].used) {
            c->owners[i].used = 1;
            c->owners[i].persistent = 0;
            c->owners[i].ep_addr = 0;
            c->owners[i].dev_addr = 0;
            c->owners[i].max_len = 0;
            c->owners[i].buffer = 0;
            return i;
        }
    }
    return -1;
}

static void td_free(uhci_ctrl_t* c, int index)
{
    if (index >= 0 && index < UHCI_TD_POOL) {
        td_init(c, index);
    }
}

/* Poll a TD until it leaves the Active state, with a timeout.
 * Returns 0 on success, negative on timeout/error/halt. */
static int td_wait(uhci_ctrl_t* c, int index, int timeout_ms)
{
    int start = timer_get_ticks();
    int target = timeout_ms / 10;
    if (target < 1) target = 1;

    while (c->tds[index].status & UHCI_TD_ACTIVE) {
        if (uhci_read(c, UHCI_USBSTS) & UHCI_STS_HCHALTED) {
            uhci_write(c, UHCI_USBSTS, UHCI_STS_HCHALTED);
            uhci_write(c, UHCI_USBCMD,
                       uhci_read(c, UHCI_USBCMD) | UHCI_CMD_RS);

            /* FIX: wait for the controller to actually leave the halted
             * state before reporting failure, otherwise the next transfer
             * immediately halts again. */
            int settle = 100;
            while ((uhci_read(c, UHCI_USBSTS) & UHCI_STS_HCHALTED) &&
                   --settle) {
                for (volatile int i = 0; i < 100; i++);
            }
            return -2;
        }
        if ((timer_get_ticks() - start) >= target) return -1;
        for (volatile int i = 0; i < 200; i++);
    }

    if (c->tds[index].status & UHCI_TD_STALLED) return -3;
    if (c->tds[index].status &
        (UHCI_TD_DBUFERR | UHCI_TD_BABBLE | UHCI_TD_CRC | UHCI_TD_BITSTUFF))
        return -4;
    return 0;
}

static int uhci_control(usb_hci_t* hci, usb_device_t* dev,
                        const usb_setup_packet_t* setup, void* data, int len,
                        int dir)
{
    uhci_ctrl_t* c = (uhci_ctrl_t*)hci->priv;
    int ls = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    int has_data = (len > 0) ? 1 : 0;

    int setup_td = td_alloc(c);
    int data_td = -1;
    int status_td = td_alloc(c);
    if (setup_td < 0 || status_td < 0) {
        if (setup_td >= 0) td_free(c, setup_td);
        if (status_td >= 0) td_free(c, status_td);
        return -1;
    }
    if (has_data) {
        data_td = td_alloc(c);
        if (data_td < 0) {
            td_free(c, setup_td);
            td_free(c, status_td);
            return -1;
        }
    }

    int pid_data = (dir == USB_DIR_IN) ? UHCI_PID_IN : UHCI_PID_OUT;
    int pid_status = (dir == USB_DIR_IN) ? UHCI_PID_OUT : UHCI_PID_IN;

    /* Status stage TD (always last). */
    c->tds[status_td].link = uhci_link_terminate();
    c->tds[status_td].status =
        UHCI_TD_ACTIVE | UHCI_TD_CERR | (ls ? UHCI_TD_LS : 0);
    c->tds[status_td].token = (pid_status & 0xFF) |
        ((dev->address & 0x7F) << 8) |
        (1 << 19) |
        (uhci_len_field(0) << 21);
    c->tds[status_td].buffer = 0;

    /* Optional data stage TD. */
    if (has_data) {
        c->tds[data_td].link = uhci_link_td(td_phys(c, status_td));
        c->tds[data_td].status = UHCI_TD_ACTIVE | UHCI_TD_CERR |
            (ls ? UHCI_TD_LS : 0) |
            ((pid_data == UHCI_PID_IN) ? UHCI_TD_SPD : 0);
        c->tds[data_td].token = (pid_data & 0xFF) |
            ((dev->address & 0x7F) << 8) |
            (1 << 19) |
            (uhci_len_field(len) << 21);
        c->tds[data_td].buffer = (unsigned int)data;
        c->tds[setup_td].link = uhci_link_td(td_phys(c, data_td));
    } else {
        c->tds[setup_td].link = uhci_link_td(td_phys(c, status_td));
    }

    /* Setup stage TD. */
    c->tds[setup_td].status =
        UHCI_TD_ACTIVE | UHCI_TD_CERR | (ls ? UHCI_TD_LS : 0);
    c->tds[setup_td].token = (UHCI_PID_SETUP & 0xFF) |
        ((dev->address & 0x7F) << 8) |
        (0 << 19) |
        (uhci_len_field(8) << 21);
    c->tds[setup_td].buffer = (unsigned int)setup;

    /* Schedule on the control queue head and run. */
    c->control_qh.element = uhci_link_td(td_phys(c, setup_td));
    uhci_write(c, UHCI_USBCMD, uhci_read(c, UHCI_USBCMD) | UHCI_CMD_RS);

    int rc = td_wait(c, status_td, 100);
    if (rc < 0) {
        log_printf("[UHCI] ctrl rc=%d setup[st=0x%X tok=0x%X] data[st=0x%X tok=0x%X] status[st=0x%X tok=0x%X] el=0x%X fl0=0x%X usbcmd=0x%X usbsts=0x%X\n",
                   rc,
                   c->tds[setup_td].status, c->tds[setup_td].token,
                   data_td >= 0 ? c->tds[data_td].status : 0,
                   data_td >= 0 ? c->tds[data_td].token : 0,
                   c->tds[status_td].status, c->tds[status_td].token,
                   c->control_qh.element, c->frame_list[0],
                   uhci_read(c, UHCI_USBCMD), uhci_read(c, UHCI_USBSTS));
    }

    /* FIX: instead of a fixed 10 ms sleep, wait (with a bound) until the
     * controller has actually retired the chain and the QH element pointer is
     * back to the terminate marker. */
    c->control_qh.element = uhci_link_terminate();
    int retire = 100;
    while (((c->control_qh.element & ~0xFU) != 0) && --retire) {
        for (volatile int i = 0; i < 100; i++);
    }

    td_free(c, setup_td);
    if (data_td >= 0) td_free(c, data_td);
    td_free(c, status_td);

    return rc;
}

static int uhci_intr_qh_slot(uhci_ctrl_t* c, usb_device_t* dev,
                             usb_endpoint_t* ep)
{
    for (int i = 0; i < UHCI_MAX_HID_QH; i++) {
        if (c->intr_qh_used[i] &&
            c->intr_qh_dev[i] == dev->address &&
            c->intr_qh_ep[i] == ep->address) {
            return i;
        }
    }
    for (int i = 0; i < UHCI_MAX_HID_QH; i++) {
        if (!c->intr_qh_used[i]) {
            c->intr_qh_used[i] = 1;
            c->intr_qh_dev[i] = dev->address;
            c->intr_qh_ep[i] = ep->address;
            c->intr_qh[i].head = uhci_link_terminate();
            c->intr_qh[i].element = uhci_link_terminate();
            return i;
        }
    }
    return -1;
}

/* FIX: release a HID QH slot and its persistent TD. Called when the endpoint
 * is truly dead (STALL/BABBLE/CRC/...), not on NAK. Without this, hot-unplug
 * permanently consumes one of the UHCI_MAX_HID_QH slots and one TD. */
static void uhci_intr_release(uhci_ctrl_t* c, int slot)
{
    if (slot < 0 || slot >= UHCI_MAX_HID_QH) return;

    for (int i = 0; i < UHCI_TD_POOL; i++) {
        if (c->owners[i].used && c->owners[i].persistent &&
            c->owners[i].dev_addr == c->intr_qh_dev[slot] &&
            c->owners[i].ep_addr == c->intr_qh_ep[slot]) {
            td_free(c, i);
            break;
        }
    }

    c->intr_qh[slot].head = uhci_link_terminate();
    c->intr_qh[slot].element = uhci_link_terminate();
    c->intr_qh_used[slot] = 0;
    c->intr_qh_dev[slot] = 0;
    c->intr_qh_ep[slot] = 0;
}

static int uhci_intr_in(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                        void* data, int len)
{
    uhci_ctrl_t* c = (uhci_ctrl_t*)hci->priv;
    int ls = (dev->speed == USB_SPEED_LOW) ? 1 : 0;

    int slot = uhci_intr_qh_slot(c, dev, ep);
    if (slot < 0) return -1;
    uhci_qh_t* qh = &c->intr_qh[slot];

    int idx = -1;
    for (int i = 0; i < UHCI_TD_POOL; i++) {
        if (c->owners[i].used && c->owners[i].persistent &&
            c->owners[i].dev_addr == dev->address &&
            c->owners[i].ep_addr == ep->address) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        idx = td_alloc(c);
        if (idx < 0) return -1;
        c->owners[idx].persistent = 1;
        c->owners[idx].dev_addr = dev->address;
        c->owners[idx].ep_addr = ep->address;
        c->owners[idx].max_len = len;
        c->owners[idx].buffer = c->intr_buf[slot];
        ep->hcd_priv = (void*)(unsigned int)idx;

        /* One TD per QH; the QH element pointer re-arms it every frame. */
        c->tds[idx].link = uhci_link_terminate();
        c->tds[idx].status = UHCI_TD_ACTIVE | UHCI_TD_CERR | UHCI_TD_SPD |
            (ls ? UHCI_TD_LS : 0);
        c->tds[idx].token = (UHCI_PID_IN & 0xFF) |
            ((dev->address & 0x7F) << 8) |
            ((ep->address & 0x0F) << 15) |
            ((ep->toggle & 1) << 19) |
            (uhci_len_field(len) << 21);
        c->tds[idx].buffer = (unsigned int)c->intr_buf[slot];
        qh->element = uhci_link_td(td_phys(c, idx));
        uhci_write(c, UHCI_USBCMD, uhci_read(c, UHCI_USBCMD) | UHCI_CMD_RS);
        return 0;
    }

    unsigned int st = c->tds[idx].status;
    if (st & UHCI_TD_ACTIVE) {
        /* Keep this endpoint scheduled in case the controller advanced the
         * QH element pointer to the terminate marker. */
        qh->element = uhci_link_td(td_phys(c, idx));
        return 0;
    }

    if (st & UHCI_TD_ERROR_MASK) {
        int nak = (st & UHCI_TD_NAK) ? 1 : 0;
        if (nak) {
            /* "No report yet": keep the toggle unchanged and re-arm. */
            c->tds[idx].status = UHCI_TD_ACTIVE | UHCI_TD_CERR |
                UHCI_TD_SPD | (ls ? UHCI_TD_LS : 0);
            c->tds[idx].token = (UHCI_PID_IN & 0xFF) |
                ((dev->address & 0x7F) << 8) |
                ((ep->address & 0x0F) << 15) |
                ((ep->toggle & 1) << 19) |
                (uhci_len_field(len) << 21);
            c->tds[idx].buffer = (unsigned int)c->intr_buf[slot];
            qh->element = uhci_link_td(td_phys(c, idx));
            return 0;
        }

        /* FIX: a non-NAK error means the endpoint is dead (unplugged or
         * stalled). Release the QH slot and the persistent TD so a future
         * device can reuse them. */
        log_printf("[UHCI] interrupt endpoint 0x%X dev %u died (st=0x%X), "
                   "releasing slot\n", ep->address, dev->address, st);
        qh->element = uhci_link_terminate();
        ep->hcd_priv = 0;
        uhci_intr_release(c, slot);
        return -1;
    }

    int act_len = (int)(st & 0x7FF);
    act_len = (act_len == 0x7FF) ? 0 : act_len + 1;
    if (act_len > len) act_len = len;
    if (act_len > 0) usb_memcpy(data, c->intr_buf[slot], (unsigned int)act_len);
    ep->toggle ^= 1;

    c->tds[idx].status = UHCI_TD_ACTIVE | UHCI_TD_CERR | UHCI_TD_SPD |
        (ls ? UHCI_TD_LS : 0);
    c->tds[idx].token = (UHCI_PID_IN & 0xFF) |
        ((dev->address & 0x7F) << 8) |
        ((ep->address & 0x0F) << 15) |
        ((ep->toggle & 1) << 19) |
        (uhci_len_field(len) << 21);
    c->tds[idx].buffer = (unsigned int)c->intr_buf[slot];
    qh->element = uhci_link_td(td_phys(c, idx));

    return act_len;
}

static int uhci_bulk(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                     void* data, int len, int dir)
{
    (void)hci; (void)dev; (void)ep; (void)data; (void)len; (void)dir;
    return -1;
}

static int uhci_port_reset(usb_hci_t* hci, int port)
{
    uhci_ctrl_t* c = (uhci_ctrl_t*)hci->priv;
    unsigned short reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    unsigned short sc = uhci_read(c, reg);

    if (!(sc & UHCI_PORT_CCS)) return -1;

    /* Clear stale connect/enable change bits before resetting. */
    uhci_write(c, reg, (unsigned short)(sc | UHCI_PORT_WC));
    sleep_ms(10);

    /* The UHCI port reset bit is bit 9 and must stay asserted for 50ms. */
    sc = uhci_read(c, reg);
    uhci_write(c, reg,
               (unsigned short)((sc & ~UHCI_PORT_WC) | UHCI_PORT_PR));
    sleep_ms(50);

    sc = uhci_read(c, reg);
    uhci_write(c, reg, (unsigned short)(sc & ~UHCI_PORT_PR));
    sleep_ms(20);

    /* Clear connect/enable changes caused by the reset. */
    sc = uhci_read(c, reg);
    uhci_write(c, reg, (unsigned short)(sc | UHCI_PORT_WC));
    sleep_ms(10);

    sc = uhci_read(c, reg);
    int speed = (sc & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;

    /* FIX: only write PE when the port is not already enabled. Full-speed
     * devices are enabled by hardware after reset; writing PE again can
     * cause PEC to bounce on some chipsets. */
    if (!(sc & UHCI_PORT_PE)) {
        uhci_write(c, reg, (unsigned short)(sc | UHCI_PORT_PE));
        sleep_ms(10);
    }

    return speed;
}

static void uhci_poll_hci(usb_hci_t* hci)
{
    uhci_ctrl_t* c = (uhci_ctrl_t*)hci->priv;
    if (uhci_read(c, UHCI_USBSTS) & UHCI_STS_HCHALTED) {
        uhci_write(c, UHCI_USBSTS, UHCI_STS_HCHALTED);
        uhci_write(c, UHCI_USBCMD, uhci_read(c, UHCI_USBCMD) | UHCI_CMD_RS);
    }
}

void uhci_poll(void)
{
    for (int i = 0; i < g_uhci_count; i++) uhci_poll_hci(&g_uhci[i].hci);
}

static void uhci_detect_ports(uhci_ctrl_t* c)
{
    /* FIX: scan in ascending order; behaviour is otherwise identical. */
    for (int port = 0; port < UHCI_MAX_PORTS; port++) {
        unsigned short reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        unsigned short sc = uhci_read(c, reg);

        if (!(sc & UHCI_PORT_CCS)) {
            if (sc & UHCI_PORT_WC) {
                uhci_write(c, reg, (unsigned short)(sc | UHCI_PORT_WC));
            }
            continue;
        }

        int speed = c->hci.port_reset(&c->hci, port);
        if (speed <= 0) {
            log_printf("[UHCI] port %d: reset failed (PORTSC=0x%X)\n",
                       port, uhci_read(c, reg));
            continue;
        }

        log_printf("[UHCI] port %d: device attached (%s speed)\n", port,
                   speed == USB_SPEED_LOW ? "low" : "full");
        usb_enumerate(&c->hci, port, speed);
    }
}

int uhci_probe(const pci_device_t* dev)
{
    if (g_uhci_count >= 4) return -1;

    uhci_ctrl_t* c = &g_uhci[g_uhci_count];
    usb_memset(c, 0, sizeof(*c));

    int io_bar = -1;
    for (int bar = 0; bar < PCI_BAR_COUNT; bar++) {
        if (dev->bar_valid[bar] && dev->bar_is_io[bar]) {
            io_bar = bar;
            break;
        }
    }
    if (io_bar < 0) {
        log_printf("[UHCI] no valid I/O BAR (bus %u slot %u)\n",
                   dev->bus, dev->slot);
        return -1;
    }
    c->io_base = (unsigned short)(dev->bar[io_bar] & ~3U);

    /* Linux clears the UHCI legacy register (PIRQ/SMI + R/WC bits) before
     * resetting the controller. Doing this after a CSM has armed its SMM
     * legacy path is what prevents the firmware and the kernel from fighting
     * over the controller during boot. */
    unsigned int legsup = pci_config_read(dev->bus, dev->slot, dev->function,
                                          UHCI_LEGACY_SUP);
    if ((legsup & 0xFFFFU) & ~(UHCI_LEGACY_RO | UHCI_LEGACY_RWC)) {
        log_printf("[UHCI] firmware legacy USB active (legsup=0x%X), leaving "
                   "controller untouched\n", legsup & 0xFFFFU);
        return -1;
    }
    pci_config_write(dev->bus, dev->slot, dev->function, UHCI_LEGACY_SUP,
                     (legsup & 0xFFFF0000U) | UHCI_LEGACY_RWC);

    pci_enable_io(dev);
    pci_enable_bus_master(dev);

    for (int i = 0; i < UHCI_TD_POOL; i++) td_init(c, i);

    unsigned int* frame_list = (unsigned int*)pmm_alloc_pages_aligned(1, 1);
    if (!frame_list) {
        log_printf("[UHCI] frame list allocation failed\n");
        return -1;
    }
    c->frame_list = frame_list;
    c->frame_list_phys = (unsigned int)frame_list;

    for (int i = 0; i < UHCI_MAX_HID_QH; i++) {
        c->intr_qh[i].head = (i + 1 < UHCI_MAX_HID_QH)
            ? uhci_link_qh((unsigned int)&c->intr_qh[i + 1])
            : uhci_link_terminate();
        c->intr_qh[i].element = uhci_link_terminate();
        c->intr_qh_used[i] = 0;
        c->intr_qh_dev[i] = 0;
        c->intr_qh_ep[i] = 0;
    }
    c->control_qh.head = uhci_link_qh((unsigned int)&c->intr_qh[0]);
    c->control_qh.element = uhci_link_terminate();

    /* The control queue is processed first, then every HID queue. */
    for (int i = 0; i < 1024; i++) {
        frame_list[i] = uhci_link_qh((unsigned int)&c->control_qh);
    }

    /* Use HCRESET, not GRESET: HCRESET resets only the host controller and
     * leaves the root hub/port state in a well-defined condition, matching
     * the sequence used by Linux's UHCI quirk. GRESET also resets the root
     * hub and can interact badly with a CSM that still has legacy SMM state. */
    uhci_write(c, UHCI_USBCMD, UHCI_CMD_HCRESET);
    int reset_wait = 100000;
    while ((uhci_read(c, UHCI_USBCMD) & UHCI_CMD_HCRESET) && --reset_wait) {
        for (volatile int i = 0; i < 10; i++);
    }
    uhci_write(c, UHCI_USBCMD, 0);
    uhci_write(c, UHCI_USBINTR, 0);
    uhci_write(c, UHCI_USBSTS, 0x3F);

    unsigned int flp = c->frame_list_phys;
    uhci_write(c, UHCI_FRBASEADD, (unsigned short)(flp & 0xFFFF));
    uhci_write(c, UHCI_FRBASEADD + 2, (unsigned short)(flp >> 16));
    uhci_write(c, UHCI_SOFMOD, 64);

    uhci_write(c, UHCI_USBCMD, UHCI_CMD_CF | UHCI_CMD_MAXP);
    uhci_write(c, UHCI_USBINTR, 0);
    uhci_write(c, UHCI_USBCMD, uhci_read(c, UHCI_USBCMD) | UHCI_CMD_RS);

    c->hci.name = "UHCI";
    c->hci.priv = c;
    c->hci.control = uhci_control;
    c->hci.intr_in = uhci_intr_in;
    c->hci.bulk = uhci_bulk;
    c->hci.port_reset = uhci_port_reset;
    c->hci.poll = uhci_poll_hci;
    c->hci.supports_hubs = 1;

    usb_register_hci(&c->hci);
    g_uhci_count++;

    log_printf("[UHCI] controller at I/O 0x%X (bus %u slot %u)\n",
               c->io_base, dev->bus, dev->slot);

    uhci_detect_ports(c);
    return 0;
}