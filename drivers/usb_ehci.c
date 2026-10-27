#include "usb_ehci.h"
#include "usb_core.h"
#include "port.h"
#include "mm.h"
#include "log.h"
#include "timer.h"

/* ---- EHCI capability registers ---- */
#define EHCI_CAPLENGTH  0x00
#define EHCI_HCSPARAMS  0x04
#define EHCI_HCCPARAMS  0x08

/* ---- EHCI operational registers (offsets from op base) ---- */
#define EHCI_USBCMD       0x00
#define EHCI_USBSTS       0x04
#define EHCI_USBINTR      0x08
#define EHCI_FRINDEX      0x0C
#define EHCI_CTRLDSSEG    0x10
#define EHCI_PERIODICLIST 0x14
#define EHCI_ASYNCLIST    0x18
#define EHCI_CONFIGFLAG   0x40
#define EHCI_PORTSC0      0x44

/* USBCMD bits */
#define EHCI_CMD_RS   0x0001
#define EHCI_CMD_HCRESET 0x0002
#define EHCI_CMD_FLS  0x0004
#define EHCI_CMD_PSE  0x0010
#define EHCI_CMD_ASE  0x0020
#define EHCI_CMD_IAAD 0x00010000

/* USBSTS bits */
#define EHCI_STS_USBINT    0x0001
#define EHCI_STS_USBERRINT 0x0002
#define EHCI_STS_PCD       0x0004
#define EHCI_STS_AS        0x0020
#define EHCI_STS_PS        0x0040
#define EHCI_STS_HCHALTED  0x1000

/* PORTSC bits */
#define EHCI_PORT_CCS   0x00000001
#define EHCI_PORT_CSC   0x00000002
#define EHCI_PORT_PE    0x00000004
#define EHCI_PORT_PEC   0x00000008
#define EHCI_PORT_PR    0x00000100
#define EHCI_PORT_PP    0x00001000
#define EHCI_PORT_OWNER 0x00002000   /* 1 = companion controller owns port */
#define EHCI_PORT_SPEED 0x0C000000
#define EHCI_PORT_SPEED_SHIFT 26

/* ---- qTD token fields ---- */
#define EHCI_TD_ACTIVE  0x00000080
#define EHCI_TD_HALTED  0x00000040
#define EHCI_TD_BUFERR  0x00000020
#define EHCI_TD_BABBLE  0x00000010
#define EHCI_TD_XACT    0x00000008
#define EHCI_TD_IOC     0x00008000
#define EHCI_TD_CERR    0x00000C00   /* bits 10-11 */
#define EHCI_TD_DT      0x80000000U  /* data toggle (DATA1) */

/* EHCI qTD PID encoding: 0 = OUT, 1 = IN, 2 = SETUP. */
#define EHCI_PID_OUT    0
#define EHCI_PID_IN     1
#define EHCI_PID_SETUP  2

/* ---- Queue Element Transfer Descriptor (32 bytes) ---- */
typedef struct {
    volatile unsigned int next;
    volatile unsigned int alt;
    volatile unsigned int token;
    volatile unsigned int buf[5];
} __attribute__((aligned(32))) ehci_qtd_t;

/* ---- Queue Head (48 bytes for 32-bit addressing) ---- */
typedef struct {
    volatile unsigned int hlp;
    volatile unsigned int epchar;
    volatile unsigned int epcap;
    volatile unsigned int cur_qtd;
    volatile unsigned int next_qtd;
    volatile unsigned int alt_qtd;
    volatile unsigned int token;
    volatile unsigned int buf[5];
} __attribute__((aligned(32))) ehci_qh_t;

#define EHCI_QTD_POOL 32
#define EHCI_MAX_PORTS 8
#define EHCI_MAX_HIDS 4

typedef struct {
    int used;
    int qh;
    int qtd;
    usb_device_t* dev;
    usb_endpoint_t* ep;
    unsigned char* buf;
    int len;
} ehci_hid_t;

typedef struct {
    volatile unsigned int* cap;
    volatile unsigned int* op;
    int num_ports;
    int is_64bit;

    unsigned int* periodic_list;   /* 1024 entries, 4KB aligned */

    ehci_qh_t async_qh;
    ehci_qtd_t qtds[EHCI_QTD_POOL];
    int qtd_used[EHCI_QTD_POOL];

    ehci_qh_t hid_qh[EHCI_MAX_HIDS];
    ehci_hid_t hids[EHCI_MAX_HIDS];
    unsigned char hid_buf[EHCI_MAX_HIDS][64];

    usb_hci_t hci;
} ehci_ctrl_t;

static ehci_ctrl_t g_ehci[4];
static int g_ehci_count = 0;

static unsigned int ehci_read32(ehci_ctrl_t* c, volatile unsigned int* reg)
{
    (void)c;
    return *reg;
}

static void ehci_write32(ehci_ctrl_t* c, volatile unsigned int* reg,
                         unsigned int val)
{
    (void)c;
    *reg = val;
}

static void ehci_qtd_init(ehci_ctrl_t* c, int index)
{
    c->qtds[index].next = 1;  /* terminate */
    c->qtds[index].alt = 1;
    c->qtds[index].token = 0;
    for (int i = 0; i < 5; i++) c->qtds[index].buf[i] = 0;
    c->qtd_used[index] = 0;
}

static int ehci_qtd_alloc(ehci_ctrl_t* c)
{
    for (int i = 0; i < EHCI_QTD_POOL; i++) {
        if (!c->qtd_used[i]) {
            c->qtd_used[i] = 1;
            return i;
        }
    }
    return -1;
}

static void ehci_qtd_free(ehci_ctrl_t* c, int index)
{
    if (index >= 0 && index < EHCI_QTD_POOL) ehci_qtd_init(c, index);
}

/* Fill the five 4KB page pointers a qTD uses for one transfer. */
static void ehci_qtd_set_buffers(ehci_ctrl_t* c, int index, void* data,
                                 int len)
{
    for (int i = 0; i < 5; i++) c->qtds[index].buf[i] = 0;
    if (!data || len <= 0) return;

    unsigned int addr = (unsigned int)data;
    unsigned int end = addr + (unsigned int)len;
    c->qtds[index].buf[0] = addr;

    int slot = 1;
    for (unsigned int page = (addr & ~0xFFFU) + 0x1000U;
         page < end && slot < 5; page += 0x1000U) {
        c->qtds[index].buf[slot++] = page;
    }
}

static int ehci_td_wait(ehci_ctrl_t* c, int index, int timeout_ms)
{
    int start = timer_get_ticks();
    int target = timeout_ms / 10;
    if (target < 1) target = 1;

    while (c->qtds[index].token & EHCI_TD_ACTIVE) {
        if (ehci_read32(c, c->op + EHCI_USBSTS / 4) & EHCI_STS_HCHALTED)
            return -2;
        if ((timer_get_ticks() - start) >= target) return -1;
        for (volatile int i = 0; i < 200; i++);
    }

    if (c->qtds[index].token &
        (EHCI_TD_HALTED | EHCI_TD_BABBLE | EHCI_TD_XACT | EHCI_TD_BUFERR))
        return -3;
    return 0;
}

/* Rebuild the EHCI periodic schedule: every frame points at the first HID QH
 * and the QHs are chained through their horizontal link pointers. */
static void ehci_periodic_rebuild(ehci_ctrl_t* c)
{
    if (!c->periodic_list) return;

    unsigned int cmd = ehci_read32(c, c->op + EHCI_USBCMD / 4);
    ehci_write32(c, c->op + EHCI_USBCMD / 4, cmd & ~EHCI_CMD_PSE);

    unsigned int first = 0;
    unsigned int prev = 0;
    for (int i = 0; i < EHCI_MAX_HIDS; i++) {
        if (!c->hids[i].used) continue;

        ehci_qh_t* qh = &c->hid_qh[i];
        qh->hlp = 1;
        /* EHCI link pointers use bits 1-2 as the type field: QH = 1. */
        unsigned int qhp = ((unsigned int)qh & ~0x1FU) | 2U;
        if (!first) first = qhp;
        else if (prev) ((ehci_qh_t*)prev)->hlp = qhp;
        prev = qhp;
    }

    for (int i = 0; i < 1024; i++) {
        c->periodic_list[i] = first ? first : 1;
    }

    ehci_write32(c, c->op + EHCI_USBCMD / 4,
                 (ehci_read32(c, c->op + EHCI_USBCMD / 4) & ~EHCI_CMD_PSE) |
                 EHCI_CMD_PSE);
}

/* Arm one interrupt qTD. The report buffer is controller-owned so the qTD
 * remains valid after the HID poll call returns. */
static void ehci_hid_arm(ehci_ctrl_t* c, ehci_hid_t* h)
{
    ehci_qtd_t* qtd = &c->qtds[h->qtd];
    qtd->next = 1;
    qtd->alt = 1;
    qtd->token = EHCI_TD_ACTIVE | EHCI_TD_CERR |
                 ((unsigned int)EHCI_PID_IN << 8) |
                 ((unsigned int)h->len << 16);
    ehci_qtd_set_buffers(c, h->qtd, h->buf, h->len);

    ehci_qh_t* qh = &c->hid_qh[h->qh];
    qh->cur_qtd = 0;
    qh->next_qtd = (unsigned int)qtd;
    qh->alt_qtd = 1;
    qh->token = 0;
}

static int ehci_intr_in(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                        void* data, int len)
{
    ehci_ctrl_t* c = (ehci_ctrl_t*)hci->priv;
    if (!data || len <= 0 || !ep || !dev) return 0;

    ehci_hid_t* h = 0;
    for (int i = 0; i < EHCI_MAX_HIDS; i++) {
        if (c->hids[i].used && c->hids[i].dev == dev &&
            c->hids[i].ep == ep) {
            h = &c->hids[i];
            break;
        }
    }

    if (!h) {
        for (int i = 0; i < EHCI_MAX_HIDS; i++) {
            if (c->hids[i].used) continue;

            int qtd = ehci_qtd_alloc(c);
            if (qtd < 0) return -1;

            h = &c->hids[i];
            usb_memset(h, 0, sizeof(*h));
            h->used = 1;
            h->qh = i;
            h->qtd = qtd;
            h->dev = dev;
            h->ep = ep;
            h->buf = c->hid_buf[i];
            h->len = ep->max_packet ? ep->max_packet : 8;
            if (h->len > (int)sizeof(c->hid_buf[i]))
                h->len = (int)sizeof(c->hid_buf[i]);

            unsigned int speed_bits = (dev->speed == USB_SPEED_HIGH) ? 2 :
                                      ((dev->speed == USB_SPEED_LOW) ? 1 : 0);
            ehci_qh_t* qh = &c->hid_qh[i];
            qh->hlp = 1;
            qh->epchar = (unsigned int)(dev->address & 0x7F) |
                         ((unsigned int)(ep->address & 0x0F) << 8) |
                         (speed_bits << 12) |       /* EPS */
                         (0U << 14) |               /* DTC: hardware toggle */
                         (0U << 15) |               /* periodic QH */
                         (((unsigned int)h->len & 0x7FFU) << 16) |
                         (0U << 27);                /* C=0 */
            qh->epcap = 0;
            qh->cur_qtd = 0;
            qh->next_qtd = 1;
            qh->alt_qtd = 1;
            qh->token = 0;
            for (int j = 0; j < 5; j++) qh->buf[j] = 0;

            ehci_hid_arm(c, h);
            ehci_periodic_rebuild(c);
            return 0;
        }
        return -1;
    }

    ehci_qtd_t* qtd = &c->qtds[h->qtd];
    unsigned int token = qtd->token;
    if (token & EHCI_TD_ACTIVE) return 0;

    if (token & (EHCI_TD_HALTED | EHCI_TD_BABBLE | EHCI_TD_XACT |
                 EHCI_TD_BUFERR)) {
        ehci_hid_arm(c, h);
        return 0;
    }

    int remaining = (int)((token >> 16) & 0x7FFF);
    int actual = h->len - remaining;
    if (actual < 0) actual = 0;
    if (actual > len) actual = len;
    if (actual > 0) usb_memcpy(data, h->buf, (unsigned int)actual);

    ehci_hid_arm(c, h);
    return actual;
}

static int ehci_control(usb_hci_t* hci, usb_device_t* dev,
                        const usb_setup_packet_t* setup, void* data, int len,
                        int dir)
{
    ehci_ctrl_t* c = (ehci_ctrl_t*)hci->priv;
    int has_data = (len > 0) ? 1 : 0;

    int setup_qtd = ehci_qtd_alloc(c);
    int data_qtd = has_data ? ehci_qtd_alloc(c) : -1;
    int status_qtd = ehci_qtd_alloc(c);
    if (setup_qtd < 0 || status_qtd < 0) {
        if (setup_qtd >= 0) ehci_qtd_free(c, setup_qtd);
        if (data_qtd >= 0) ehci_qtd_free(c, data_qtd);
        if (status_qtd >= 0) ehci_qtd_free(c, status_qtd);
        return -1;
    }
    if (has_data && data_qtd < 0) {
        ehci_qtd_free(c, setup_qtd);
        ehci_qtd_free(c, status_qtd);
        return -1;
    }

    int pid_data = (dir == USB_DIR_IN) ? EHCI_PID_IN : EHCI_PID_OUT;
    int pid_status = (dir == USB_DIR_IN) ? EHCI_PID_OUT : EHCI_PID_IN;

    /* Status stage (last): zero-length DATA1 packet. */
    c->qtds[status_qtd].next = 1;   /* terminate */
    c->qtds[status_qtd].alt = 1;
    c->qtds[status_qtd].token = EHCI_TD_ACTIVE | EHCI_TD_CERR | EHCI_TD_DT |
        (unsigned int)(pid_status << 8);
    ehci_qtd_set_buffers(c, status_qtd, 0, 0);

    /* Data stage (optional): DATA1, with page pointers for DMA. */
    if (has_data) {
        c->qtds[data_qtd].next = (unsigned int)&c->qtds[status_qtd];
        c->qtds[data_qtd].alt = 1;
        c->qtds[data_qtd].token = EHCI_TD_ACTIVE | EHCI_TD_CERR | EHCI_TD_DT |
            (unsigned int)(pid_data << 8) |
            ((unsigned int)len << 16);
        ehci_qtd_set_buffers(c, data_qtd, data, len);
        c->qtds[setup_qtd].next = (unsigned int)&c->qtds[data_qtd];
    } else {
        c->qtds[setup_qtd].next = (unsigned int)&c->qtds[status_qtd];
    }

    /* Setup stage (first): DATA0, exactly 8 bytes. */
    c->qtds[setup_qtd].alt = 1;
    c->qtds[setup_qtd].token = EHCI_TD_ACTIVE | EHCI_TD_CERR |
        (unsigned int)(EHCI_PID_SETUP << 8) | (8U << 16);
    ehci_qtd_set_buffers(c, setup_qtd, (void*)setup, 8);

    /* Configure the async queue head for this device (EP0, control). */
    unsigned int speed_bits;
    if (dev->speed == USB_SPEED_LOW) speed_bits = 1;
    else if (dev->speed == USB_SPEED_HIGH) speed_bits = 2;
    else speed_bits = 0;

    c->async_qh.hlp = 1;   /* terminate list */
    c->async_qh.epchar = (unsigned int)(dev->address & 0x7F) |
        (0U << 9) |
        (speed_bits << 12) |
        (0U << 14) |        /* DTC=0: hardware manages toggle */
        (1U << 15) |        /* H=1 (head of list) */
        ((dev->max_packet0 & 0x7FFU) << 16) |
        (1U << 27);         /* C=1 (control endpoint) */
    c->async_qh.epcap = 0;
    c->async_qh.cur_qtd = 0;
    c->async_qh.next_qtd = (unsigned int)&c->qtds[setup_qtd];
    c->async_qh.alt_qtd = 1;
    c->async_qh.token = 0;
    for (int i = 0; i < 5; i++) c->async_qh.buf[i] = 0;

    ehci_write32(c, c->op + EHCI_ASYNCLIST / 4,
                 (unsigned int)&c->async_qh);
    ehci_write32(c, c->op + EHCI_USBCMD / 4,
                 ehci_read32(c, c->op + EHCI_USBCMD / 4) |
                 EHCI_CMD_ASE | EHCI_CMD_RS);

    int rc = ehci_td_wait(c, status_qtd, 100);

    if (rc < 0) {
        log_printf("[EHCI] control transfer failed (rc=%d)\n", rc);
    }

    ehci_write32(c, c->op + EHCI_USBCMD / 4,
                 ehci_read32(c, c->op + EHCI_USBCMD / 4) & ~EHCI_CMD_ASE);
    c->async_qh.next_qtd = 1;

    ehci_qtd_free(c, setup_qtd);
    if (data_qtd >= 0) ehci_qtd_free(c, data_qtd);
    ehci_qtd_free(c, status_qtd);

    return rc;
}

static int ehci_port_reset(usb_hci_t* hci, int port)
{
    ehci_ctrl_t* c = (ehci_ctrl_t*)hci->priv;
    volatile unsigned int* portsc = c->op + EHCI_PORTSC0 / 4 + port;

    unsigned int sc = ehci_read32(c, portsc);
    if (!(sc & EHCI_PORT_CCS)) return -1;

    /* A port already owned by a companion must not be touched by EHCI. */
    if (sc & EHCI_PORT_OWNER) return -1;

    if (sc & (EHCI_PORT_CSC | EHCI_PORT_PEC)) {
        ehci_write32(c, portsc,
                     sc | EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_PP);
    }

    sc = ehci_read32(c, portsc);
    ehci_write32(c, portsc, sc | EHCI_PORT_PR | EHCI_PORT_PP);
    sleep_ms(50);

    sc = ehci_read32(c, portsc);
    ehci_write32(c, portsc, (sc & ~EHCI_PORT_PR) | EHCI_PORT_PP);
    sleep_ms(20);

    sc = ehci_read32(c, portsc);
    ehci_write32(c, portsc,
                 sc | EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_PP);
    sleep_ms(10);

    sc = ehci_read32(c, portsc);
    int speed = (sc & EHCI_PORT_SPEED) >> EHCI_PORT_SPEED_SHIFT;

    /* After reset, high-speed devices show PED=1. Some controllers (and QEMU)
     * do not implement the PORTSC speed field, so PED is the reliable bit. */
    if ((sc & EHCI_PORT_PE) || speed == 2) {
        ehci_write32(c, portsc, sc | EHCI_PORT_PE | EHCI_PORT_PP);
        sleep_ms(10);
        return USB_SPEED_HIGH;
    }

    /* Full/low-speed devices cannot run on an EHCI root port: give the port
     * back to its companion controller (UHCI). */
    ehci_write32(c, portsc, sc | EHCI_PORT_OWNER | EHCI_PORT_PP);
    sleep_ms(10);
    return -1;
}

static void ehci_poll_hci(usb_hci_t* hci)
{
    ehci_ctrl_t* c = (ehci_ctrl_t*)hci->priv;
    if (ehci_read32(c, c->op + EHCI_USBSTS / 4) & EHCI_STS_HCHALTED) {
        ehci_write32(c, c->op + EHCI_USBSTS / 4, EHCI_STS_HCHALTED);
        ehci_write32(c, c->op + EHCI_USBCMD / 4,
                     ehci_read32(c, c->op + EHCI_USBCMD / 4) | EHCI_CMD_RS);
    }
}

void ehci_poll(void)
{
    for (int i = 0; i < g_ehci_count; i++) ehci_poll_hci(&g_ehci[i].hci);
}

/* Take the controller away from firmware legacy USB support.
 *
 * CSM firmware with "Legacy USB Support" keeps EHCI under BIOS ownership and
 * services the USB keyboard/mouse through SMI. Resetting such a controller
 * without clearing the BIOS semaphore first makes firmware and OS fight over
 * the ports: the devices are no longer serviced, they suspend and both LEDs go
 * out the moment the desktop appears.
 *
 * Returns 1 when the OS may drive the controller, 0 when firmware still owns
 * the handoff and the caller must leave the hardware completely untouched. */
static int ehci_legacy_handoff(const pci_device_t* dev)
{
    unsigned int hccparams = pci_config_read(dev->bus, dev->slot,
                                             dev->function, EHCI_HCCPARAMS);
    unsigned char eecp = (unsigned char)((hccparams >> 8) & 0xFF);
    int hops = 0;

    while (eecp >= 0x40 && hops++ < 32) {
        unsigned int cap = pci_config_read(dev->bus, dev->slot, dev->function,
                                           eecp);

        if ((cap & 0xFFU) != 0x01U) {   /* not the USBLEGSUP capability */
            eecp = (unsigned char)((cap >> 8) & 0xFF);
            continue;
        }

        /* Bit 16 = HC BIOS Owned semaphore, bit 17 = HC OS Owned semaphore.
         * Bits 20 and 23 are write-1-to-clear SMI enables. */
        if (!(cap & 0x00010000U) && (cap & 0x00020000U)) return 1;

        pci_config_write(dev->bus, dev->slot, dev->function, eecp,
                         cap | 0x00020000U | 0x00900000U);

        int released = 0;
        for (int i = 0; i < 20; i++) {
            if (!(pci_config_read(dev->bus, dev->slot, dev->function, eecp) &
                  0x00010000U)) {
                released = 1;
                break;
            }
            sleep_ms(10);
        }
        if (!released) {
            log_printf("[EHCI] BIOS still owns legacy USB (bus %u slot %u), "
                       "leaving the controller alone\n", dev->bus, dev->slot);
            return 0;
        }

        /* USBLEGCTLSTS: turn off every legacy SMI source. */
        pci_config_write(dev->bus, dev->slot, dev->function,
                         (unsigned char)(eecp + 4), 0);
        log_printf("[EHCI] legacy USB ownership taken over from firmware\n");
        return 1;
    }

    return 1;   /* no legacy capability block, nothing to hand over */
}

static void ehci_detect_ports(ehci_ctrl_t* c)
{
    for (int port = 0; port < c->num_ports && port < EHCI_MAX_PORTS; port++) {
        volatile unsigned int* portsc = c->op + EHCI_PORTSC0 / 4 + port;
        unsigned int sc = ehci_read32(c, portsc);

        /* Clear stale connect/enable change bits. */
        if (sc & (EHCI_PORT_CSC | EHCI_PORT_PEC)) {
            ehci_write32(c, portsc,
                         sc | EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_PP);
            sc = ehci_read32(c, portsc);
        }
        if (!(sc & EHCI_PORT_CCS)) continue;
        if (sc & EHCI_PORT_OWNER) continue;   /* companion already owns it */

        /* Reset first: PORTSC speed bits are only reliable afterwards. */
        int speed = c->hci.port_reset(&c->hci, port);
        if (speed != USB_SPEED_HIGH) {
            log_printf("[EHCI] port %d: full/low-speed device handed to "
                       "companion\n", port);
            continue;
        }

        log_printf("[EHCI] port %d: device attached (high speed)\n", port);
        usb_enumerate(&c->hci, port, speed);
    }
}

int ehci_probe(const pci_device_t* dev)
{
    if (g_ehci_count >= 4) return -1;

    ehci_ctrl_t* c = &g_ehci[g_ehci_count];
    usb_memset(c, 0, sizeof(*c));

    if (!dev->bar_valid[0] || dev->bar_is_io[0]) {
        log_printf("[EHCI] no MMIO BAR (bus %u slot %u)\n", dev->bus, dev->slot);
        return -1;
    }

    c->cap = (volatile unsigned int*)dev->bar[0];
    unsigned char cap_len = *(volatile unsigned char*)dev->bar[0];
    c->op = (volatile unsigned int*)(dev->bar[0] + cap_len);

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);

    unsigned int hcsparams = ehci_read32(c, c->cap + EHCI_HCSPARAMS / 4);
    unsigned int hccparams = ehci_read32(c, c->cap + EHCI_HCCPARAMS / 4);
    c->num_ports = (int)(hcsparams & 0xFU);
    c->is_64bit = (hccparams & 1U) ? 1 : 0;
    int companions = (int)((hcsparams >> 12) & 0xFU);

    if (c->num_ports <= 0) {
        log_printf("[EHCI] controller reports no ports (bus %u slot %u)\n",
                   dev->bus, dev->slot);
        return -1;
    }
    if (c->num_ports > EHCI_MAX_PORTS) c->num_ports = EHCI_MAX_PORTS;

    /* A 32-bit kernel can drive a 64-bit capable EHCI as long as all queue
     * memory lives below 4GB: CTRLDSSEGMENT stays zero. */
    if (c->is_64bit) {
        log_printf("[EHCI] 64-bit capable controller, using 32-bit addressing\n");
    }

    /* Firmware that still owns the controller through legacy USB support must
     * keep it: resetting it would cut off the USB keyboard/mouse that the BIOS
     * is emulating as PS/2. */
    if (!ehci_legacy_handoff(dev)) return -1;

    for (int i = 0; i < EHCI_QTD_POOL; i++) ehci_qtd_init(c, i);

    unsigned int* plist = (unsigned int*)pmm_alloc_pages_aligned(2, 1);
    if (!plist) {
        log_printf("[EHCI] periodic list allocation failed\n");
        return -1;
    }
    c->periodic_list = plist;
    for (int i = 0; i < 1024; i++) plist[i] = 1;  /* terminate */

    ehci_write32(c, c->op + EHCI_USBCMD / 4, EHCI_CMD_HCRESET);
    int timeout = 1000000;
    while ((ehci_read32(c, c->op + EHCI_USBCMD / 4) & EHCI_CMD_HCRESET) &&
           --timeout) {
        for (volatile int i = 0; i < 10; i++);
    }

    ehci_write32(c, c->op + EHCI_CTRLDSSEG / 4, 0);
    ehci_write32(c, c->op + EHCI_PERIODICLIST / 4, (unsigned int)plist);

    c->async_qh.hlp = 1;
    c->async_qh.epchar = 0;
    c->async_qh.epcap = 0;
    c->async_qh.cur_qtd = 0;
    c->async_qh.next_qtd = 1;
    c->async_qh.alt_qtd = 1;
    c->async_qh.token = 0;
    for (int i = 0; i < 5; i++) c->async_qh.buf[i] = 0;

    ehci_write32(c, c->op + EHCI_ASYNCLIST / 4, (unsigned int)&c->async_qh);
    ehci_write32(c, c->op + EHCI_USBSTS / 4, 0x3F);
    ehci_write32(c, c->op + EHCI_USBINTR / 4, 0);

    ehci_write32(c, c->op + EHCI_USBCMD / 4,
                 EHCI_CMD_PSE | EHCI_CMD_RS);
    ehci_write32(c, c->op + EHCI_CONFIGFLAG / 4, 1);

    /* Power every root port before probing for devices. On controllers with
     * port-power control this is required to see anything at all. */
    for (int port = 0; port < c->num_ports; port++) {
        volatile unsigned int* portsc = c->op + EHCI_PORTSC0 / 4 + port;
        unsigned int sc = ehci_read32(c, portsc);
        if (!(sc & EHCI_PORT_PP)) {
            ehci_write32(c, portsc, sc | EHCI_PORT_PP);
        }
    }
    sleep_ms(50);

    c->hci.name = "EHCI";
    c->hci.priv = c;
    c->hci.control = ehci_control;
    c->hci.intr_in = ehci_intr_in;
    c->hci.bulk = 0;
    c->hci.port_reset = ehci_port_reset;
    c->hci.poll = ehci_poll_hci;

    usb_register_hci(&c->hci);
    g_ehci_count++;

    log_printf("[EHCI] controller at MMIO 0x%X, %d ports, %d companion(s)%s "
               "(bus %u slot %u)\n",
               dev->bar[0], c->num_ports, companions,
               c->is_64bit ? ", 64-bit capable" : "", dev->bus, dev->slot);

    ehci_detect_ports(c);
    return 0;
}


