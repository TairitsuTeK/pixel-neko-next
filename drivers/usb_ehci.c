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
#define EHCI_PORT_SPEED 0x0C000000
#define EHCI_PORT_SPEED_SHIFT 26

/* ---- qTD token fields ---- */
#define EHCI_TD_ACTIVE  0x00000080
#define EHCI_TD_HALTED  0x00000040
#define EHCI_TD_BUFERR  0x00000020
#define EHCI_TD_BABBLE  0x00000010
#define EHCI_TD_XACT    0x00000008
#define EHCI_TD_IOC     0x00008000
#define EHCI_TD_CERR    0x00003000

#define EHCI_PID_SETUP  0
#define EHCI_PID_OUT    1
#define EHCI_PID_IN     2

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

typedef struct {
    volatile unsigned int* cap;
    volatile unsigned int* op;
    int num_ports;
    int is_64bit;

    unsigned int* periodic_list;   /* 1024 entries, 4KB aligned */

    ehci_qh_t async_qh;
    ehci_qtd_t qtds[EHCI_QTD_POOL];
    int qtd_used[EHCI_QTD_POOL];

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

    /* Status stage (last). */
    c->qtds[status_qtd].next = 1;   /* terminate */
    c->qtds[status_qtd].alt = 1;
    c->qtds[status_qtd].token = EHCI_TD_ACTIVE | EHCI_TD_CERR |
        (unsigned int)(pid_status << 8) |
        ((pid_status == EHCI_PID_IN) ? 0x4000U << 16 : 0);
    c->qtds[status_qtd].buf[0] = 0;
    c->qtds[status_qtd].buf[1] = 0;
    c->qtds[status_qtd].buf[2] = 0;
    c->qtds[status_qtd].buf[3] = 0;
    c->qtds[status_qtd].buf[4] = 0;

    /* Data stage (optional). */
    if (has_data) {
        c->qtds[data_qtd].next = (unsigned int)&c->qtds[status_qtd];
        c->qtds[data_qtd].alt = 1;
        c->qtds[data_qtd].token = EHCI_TD_ACTIVE | EHCI_TD_CERR |
            (unsigned int)(pid_data << 8) |
            ((unsigned int)len << 16);
        c->qtds[data_qtd].buf[0] = (unsigned int)data;
        c->qtds[data_qtd].buf[1] = 0;
        c->qtds[data_qtd].buf[2] = 0;
        c->qtds[data_qtd].buf[3] = 0;
        c->qtds[data_qtd].buf[4] = 0;
        c->qtds[setup_qtd].next = (unsigned int)&c->qtds[data_qtd];
    } else {
        c->qtds[setup_qtd].next = (unsigned int)&c->qtds[status_qtd];
    }

    /* Setup stage (first). */
    c->qtds[setup_qtd].alt = 1;
    c->qtds[setup_qtd].token = EHCI_TD_ACTIVE | EHCI_TD_CERR |
        (unsigned int)(EHCI_PID_SETUP << 8) | (8U << 16);
    c->qtds[setup_qtd].buf[0] = (unsigned int)setup;
    c->qtds[setup_qtd].buf[1] = 0;
    c->qtds[setup_qtd].buf[2] = 0;
    c->qtds[setup_qtd].buf[3] = 0;
    c->qtds[setup_qtd].buf[4] = 0;

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

    if (!(ehci_read32(c, portsc) & EHCI_PORT_CCS)) return -1;

    ehci_write32(c, portsc, ehci_read32(c, portsc) | EHCI_PORT_PR);
    sleep_ms(50);
    ehci_write32(c, portsc, ehci_read32(c, portsc) & ~EHCI_PORT_PR);
    sleep_ms(10);

    unsigned int sc = ehci_read32(c, portsc);
    int speed;
    switch ((sc & EHCI_PORT_SPEED) >> EHCI_PORT_SPEED_SHIFT) {
        case 2:  speed = USB_SPEED_HIGH; break;
        case 1:  speed = USB_SPEED_LOW;  break;
        default: speed = USB_SPEED_FULL; break;
    }

    ehci_write32(c, portsc,
                 ehci_read32(c, portsc) | EHCI_PORT_PE | EHCI_PORT_PP);
    sleep_ms(5);

    return speed;
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

static void ehci_detect_ports(ehci_ctrl_t* c)
{
    for (int port = 0; port < c->num_ports && port < EHCI_MAX_PORTS; port++) {
        volatile unsigned int* portsc = c->op + EHCI_PORTSC0 / 4 + port;
        if (!(ehci_read32(c, portsc) & EHCI_PORT_CCS)) continue;

        int speed = c->hci.port_reset(&c->hci, port);
        if (speed <= 0) continue;

        const char* name = (speed == USB_SPEED_HIGH) ? "high" :
                           (speed == USB_SPEED_LOW) ? "low" : "full";
        log_printf("[EHCI] port %d: device attached (%s speed)\n", port, name);
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
    c->is_64bit = (int)(hcsparams & 1U);
    c->num_ports = (int)((hcsparams >> 12) & 0xFU);

    if (c->is_64bit) {
        log_printf("[EHCI] 64-bit EHCI not supported by this driver\n");
        return -1;
    }

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

    ehci_write32(c, c->op + EHCI_USBCMD / 4, EHCI_CMD_FLS | EHCI_CMD_RS);
    ehci_write32(c, c->op + EHCI_CONFIGFLAG / 4, 1);

    c->hci.name = "EHCI";
    c->hci.priv = c;
    c->hci.control = ehci_control;
    c->hci.intr_in = 0;   /* HID low/full-speed devices use UHCI */
    c->hci.bulk = 0;
    c->hci.port_reset = ehci_port_reset;
    c->hci.poll = ehci_poll_hci;

    usb_register_hci(&c->hci);
    g_ehci_count++;

    log_printf("[EHCI] controller at MMIO 0x%X, %d ports (bus %u slot %u)\n",
               dev->bar[0], c->num_ports, dev->bus, dev->slot);

    ehci_detect_ports(c);
    return 0;
}


