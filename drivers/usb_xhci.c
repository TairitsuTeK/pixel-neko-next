#include "usb_xhci.h"
#include "usb_core.h"
#include "port.h"
#include "mm.h"
#include "log.h"
#include "timer.h"

/* ---- Capability registers (offsets from the MMIO BAR) ---- */
#define XHCI_CAPLENGTH      0x00    /* byte  */
#define XHCI_HCIVERSION     0x02    /* short */
#define XHCI_HCSPARAMS1     0x04
#define XHCI_HCSPARAMS2     0x08
#define XHCI_HCCPARAMS1     0x10
#define XHCI_DBOFF          0x14
#define XHCI_RTSOFF         0x18

/* ---- Operational registers (offsets from the operational base) ---- */
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR           0x18
#define XHCI_DCBAAP         0x30
#define XHCI_CONFIG         0x38
#define XHCI_PORTSC         0x400

/* USBCMD bits */
#define XHCI_CMD_RS         0x00000001
#define XHCI_CMD_HCRST      0x00000002
#define XHCI_CMD_INTE       0x00000004
#define XHCI_CMD_HSEE       0x00000008
#define XHCI_CMD_EWE        0x00000400

/* USBSTS bits */
#define XHCI_STS_HCH        0x00000001
#define XHCI_STS_HSE        0x00000004
#define XHCI_STS_EINT       0x00000008
#define XHCI_STS_PCD        0x00000010
#define XHCI_STS_CNR        0x00000800
#define XHCI_STS_HCE        0x00001000

/* PORTSC bits */
#define XHCI_PORT_CCS       0x00000001
#define XHCI_PORT_PED       0x00000002
#define XHCI_PORT_OCA       0x00000008
#define XHCI_PORT_PR        0x00000010
#define XHCI_PORT_PLS_MSK   0x000001E0
#define XHCI_PORT_PLS_SH    5
#define XHCI_PORT_PP        0x00000200
#define XHCI_PORT_SPEED_MSK 0x00003C00
#define XHCI_PORT_SPEED_SH  10
#define XHCI_PORT_CSC       0x00020000
#define XHCI_PORT_PEC       0x00040000
#define XHCI_PORT_WRC       0x00080000
#define XHCI_PORT_OCC       0x00100000
#define XHCI_PORT_PRC       0x00200000
#define XHCI_PORT_PLC       0x00400000
#define XHCI_PORT_CEC       0x00800000
#define XHCI_PORT_CHANGE    (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | \
                             XHCI_PORT_OCC | XHCI_PORT_PRC | XHCI_PORT_PLC | \
                             XHCI_PORT_CEC)

/* Interrupter register set, relative to the runtime base. Offset 0x00 of the
 * runtime space is MFINDEX, so the first interrupter starts at 0x20 and every
 * following one is another 0x20 bytes further. */
#define XHCI_IMAN          0x20
#define XHCI_IMOD          0x24
#define XHCI_ERSTSZ        0x28
#define XHCI_ERSTBA        0x30
#define XHCI_ERDP          0x38

/* ---- TRB types ---- */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_ADDRESS_DEV     11
#define TRB_CONFIGURE_EP    12
#define TRB_EVALUATE_CTX    13
#define TRB_XFER_EVENT      32
#define TRB_CMD_COMPLETE    33
#define TRB_PORT_STATUS     34

#define TRB_TYPE(t)         ((unsigned int)(t) << 10)
#define TRB_CYCLE           0x00000001U
#define TRB_IOC             0x00000020U
#define TRB_IDT             0x00000040U
#define TRB_TC              0x00000002U
#define TRB_DIR_IN          0x00010000U
#define TRB_COMP_SUCCESS    1U

/* Endpoint context EP types */
#define EP_TYPE_CONTROL     4
#define EP_TYPE_INTR_IN     7

#define XHCI_MAX_SLOTS      8
#define XHCI_MAX_PORTS      32
#define XHCI_RING_TRBS      64      /* TRBs per ring (last one is the link) */
#define XHCI_EVENT_TRBS     256
#define XHCI_TRB_SIZE       16
#define XHCI_CTX_SIZE       32      /* slot/endpoint context size */

typedef struct {
    volatile unsigned int d0;
    volatile unsigned int d1;
    volatile unsigned int d2;
    volatile unsigned int d3;
} xhci_trb_t;

/* A ring is 64-byte aligned and its last TRB is a link back to the top. */
typedef struct {
    xhci_trb_t*  trbs;
    unsigned int phys;
    int          size;
    int          enqueue;
    int          cycle;
} xhci_ring_t;

typedef struct {
    int used;
    int slot_id;
    int port;               /* 0-based port index */
    int speed;
    int ep0_mps;

    xhci_ring_t ep0;
    unsigned char* dev_ctx; /* device context, 64-byte aligned */

    /* Interrupt IN endpoint (HID reports) */
    int          intr_dci;
    int          intr_configured;
    xhci_ring_t  intr;
    unsigned char* intr_buf;
    int          intr_armed;
    unsigned int intr_trb;
    int          intr_len;
    int          intr_done;
    int          intr_residual;
} xhci_slot_t;

typedef struct {
    unsigned int bar;
    volatile unsigned int* cap;
    volatile unsigned int* op;
    volatile unsigned int* rt;
    unsigned int dboff;

    int max_slots;
    int max_ports;
    int max_intrs;
    int page_size;

    unsigned int* dcbaa;
    xhci_ring_t   cmd;

    xhci_trb_t*   evt;
    unsigned int  evt_phys;
    int           evt_idx;
    int           evt_cycle;

    unsigned int* erst;
    unsigned int* in_ctx;

    xhci_slot_t   slots[XHCI_MAX_SLOTS];

    volatile int  cmd_valid;
    unsigned int  cmd_slot;
    unsigned int  cmd_code;

    /* Completion of the control-transfer currently being waited on. */
    volatile int  xfer_valid;
    unsigned int  xfer_trb;
    unsigned int  xfer_code;
    int           xfer_residual;

    usb_hci_t     hci;
} xhci_ctrl_t;

static xhci_ctrl_t g_xhci[2];
static int g_xhci_count = 0;

/* ---- Register access ---- */
static unsigned int xhci_cap_read(xhci_ctrl_t* c, unsigned int off)
{
    return *(volatile unsigned int*)(c->bar + off);
}

static unsigned int xhci_op_read(xhci_ctrl_t* c, unsigned int off)
{
    return *(volatile unsigned int*)((unsigned int)c->op + off);
}

static void xhci_op_write(xhci_ctrl_t* c, unsigned int off, unsigned int val)
{
    *(volatile unsigned int*)((unsigned int)c->op + off) = val;
}

static void xhci_rt_write(xhci_ctrl_t* c, unsigned int off, unsigned int val)
{
    *(volatile unsigned int*)((unsigned int)c->rt + off) = val;
}

static unsigned int xhci_port_read(xhci_ctrl_t* c, int port)
{
    return xhci_op_read(c, XHCI_PORTSC + (unsigned int)port * 0x10U);
}

static void xhci_port_write(xhci_ctrl_t* c, int port, unsigned int val)
{
    xhci_op_write(c, XHCI_PORTSC + (unsigned int)port * 0x10U, val);
}

/* target 0 = command ring, 1..31 = endpoint DCI of that slot. */
static void xhci_doorbell(xhci_ctrl_t* c, int slot, int target)
{
    volatile unsigned int* db = (volatile unsigned int*)(c->bar + c->dboff);
    db[slot] = (unsigned int)target;
}

/* ---- Ring helpers ---- */
static void xhci_ring_init(xhci_ring_t* r, void* page, int trbs)
{
    r->trbs = (xhci_trb_t*)page;
    r->phys = (unsigned int)page;
    r->size = trbs;
    r->enqueue = 0;
    r->cycle = 1;

    for (int i = 0; i < trbs; i++) {
        r->trbs[i].d0 = 0;
        r->trbs[i].d1 = 0;
        r->trbs[i].d2 = 0;
        r->trbs[i].d3 = 0;
    }

    /* Link TRB with TC=1 keeps the controller looping over the ring. */
    xhci_trb_t* link = &r->trbs[trbs - 1];
    link->d0 = r->phys;
    link->d1 = 0;
    link->d2 = 0;
    link->d3 = TRB_CYCLE | TRB_TC | TRB_TYPE(TRB_LINK);
}

/* Reserve the next TRB with the producer cycle bit already set. */
static xhci_trb_t* xhci_ring_push(xhci_ring_t* r)
{
    if (r->enqueue >= r->size - 1) {
        /* Publish the link TRB for the round we just finished, then wrap. */
        r->trbs[r->size - 1].d3 =
            (r->trbs[r->size - 1].d3 & ~TRB_CYCLE) | (unsigned int)r->cycle;
        r->cycle ^= 1;
        r->enqueue = 0;
    }

    xhci_trb_t* t = &r->trbs[r->enqueue];
    t->d0 = 0;
    t->d1 = 0;
    t->d2 = 0;
    t->d3 = (unsigned int)r->cycle;
    r->enqueue++;
    return t;
}

static unsigned int xhci_ring_dequeue(xhci_ring_t* r)
{
    unsigned int addr = r->phys + (unsigned int)r->enqueue * XHCI_TRB_SIZE;
    return (addr & ~0xFU) | (unsigned int)(r->cycle & 1);
}

/* ---- Event processing ---- */
static void xhci_process_events(xhci_ctrl_t* c)
{
    for (int guard = 0; guard < 128; guard++) {
        xhci_trb_t* e = &c->evt[c->evt_idx];
        if ((int)(e->d3 & TRB_CYCLE) != c->evt_cycle) break;

        unsigned int type = (e->d3 >> 10) & 0x3FU;
        unsigned int code = (e->d2 >> 24) & 0xFFU;

        if (type == TRB_CMD_COMPLETE) {
            c->cmd_slot = (e->d3 >> 24) & 0xFFU;
            c->cmd_code = code;
            c->cmd_valid = 1;
        } else if (type == TRB_XFER_EVENT) {
            unsigned int trb = e->d0 & ~0xFU;
            unsigned int ep = (e->d3 >> 16) & 0x1FU;
            int residual = (int)(e->d2 & 0xFFFFFFU);
            int done = 0;

            for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
                xhci_slot_t* s = &c->slots[i];
                if (!s->used || !s->intr_armed) continue;
                if (s->intr_dci == (int)ep && s->intr_trb == trb) {
                    s->intr_armed = 0;
                    s->intr_done = (code == TRB_COMP_SUCCESS) ? 1 : 0;
                    s->intr_residual = residual;
                    done = 1;
                }
            }

            if (!done) {
                c->xfer_trb = trb;
                c->xfer_code = code;
                c->xfer_residual = residual;
                c->xfer_valid = 1;
            }
        } else if (type == TRB_PORT_STATUS) {
            int port = (int)((e->d0 >> 24) & 0xFFU) - 1;
            if (port >= 0 && port < c->max_ports) {
                xhci_port_write(c, port, xhci_port_read(c, port));
            }
        }

        c->evt_idx++;
        if (c->evt_idx >= XHCI_EVENT_TRBS) {
            c->evt_idx = 0;
            c->evt_cycle ^= 1;
        }

        /* Hand the consumed segment back to the controller (EHB = 1). */
        xhci_rt_write(c, XHCI_ERDP,
                      (c->evt_phys +
                       (unsigned int)c->evt_idx * XHCI_TRB_SIZE) | 0x8U);
    }
}

/* Submit one command TRB and wait for its completion event. Commands other
 * than "Enable Slot" must carry the target slot id in bits 24-31 of the
 * control dword. Returns the slot id reported by the controller, or a
 * negative value on failure. */
static int xhci_cmd(xhci_ctrl_t* c, unsigned int d0, unsigned int d1,
                    unsigned int d2, unsigned int type, int slot)
{
    c->cmd_valid = 0;

    xhci_trb_t* t = xhci_ring_push(&c->cmd);
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    t->d3 = (t->d3 & TRB_CYCLE) | TRB_TYPE(type) |
            ((unsigned int)(slot & 0xFF) << 24);

    xhci_doorbell(c, 0, 0);

    int start = timer_get_ticks();
    while (!c->cmd_valid) {
        xhci_process_events(c);
        if ((timer_get_ticks() - start) >= 50) break;       /* ~500 ms */
        for (volatile int i = 0; i < 200; i++);
    }

    if (!c->cmd_valid) {
        log_printf("[XHCI] cmd %u: no completion (sts=0x%X crcr=0x%X ccs=%d)\n",
                   type, xhci_op_read(c, XHCI_USBSTS),
                   xhci_op_read(c, XHCI_CRCR), c->evt_cycle);
        return -1;
    }
    if (c->cmd_code != TRB_COMP_SUCCESS) {
        log_printf("[XHCI] cmd %u: completion code %u\n", type, c->cmd_code);
        return -2;
    }
    return (int)c->cmd_slot;
}

/* ---- Context builders (each context is 32 bytes = 8 dwords) ---- */
static void xhci_build_slot_ctx(unsigned int* ctx, int speed, int port1,
                                int entries)
{
    unsigned int speed_code = 1;                      /* full speed */
    if (speed == USB_SPEED_LOW) speed_code = 2;
    else if (speed == USB_SPEED_HIGH) speed_code = 3;

    ctx[0] = ((unsigned int)(entries & 0x1F) << 27) | (speed_code << 20);
    ctx[1] = (unsigned int)(port1 & 0xFF) << 16;
    ctx[2] = 0;
    ctx[3] = 0;
    ctx[4] = 0;
    ctx[5] = 0;
    ctx[6] = 0;
    ctx[7] = 0;
}

static void xhci_build_ep_ctx(unsigned int* ctx, int ep_type, int mps,
                              int interval, unsigned int dequeue)
{
    ctx[0] = (unsigned int)(interval & 0xFF) << 16;
    ctx[1] = (3U << 1) |                                /* CErr = 3 */
             ((unsigned int)(ep_type & 0x7) << 3) |
             ((unsigned int)(mps & 0x7FF) << 16);
    /* Dequeue pointer: 16-byte aligned address with DCS (bit 0) set. */
    ctx[2] = dequeue;
    ctx[3] = 0;
    ctx[4] = 8;                                         /* avg TRB length */
    ctx[5] = 0;
    ctx[6] = 0;
    ctx[7] = 0;
}

static xhci_slot_t* xhci_slot_for_port(xhci_ctrl_t* c, int port)
{
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (c->slots[i].used && c->slots[i].port == port) return &c->slots[i];
    }
    return 0;
}

static int xhci_highest_dci(xhci_slot_t* s)
{
    return (s->intr_configured && s->intr_dci > 1) ? s->intr_dci : 1;
}

/* Address Device: hands the slot its USB address and activates EP0. */
static int xhci_address_device(xhci_ctrl_t* c, xhci_slot_t* s, int mps)
{
    unsigned int* in = c->in_ctx;
    usb_memset(in, 0, 33 * XHCI_CTX_SIZE);

    in[1] = 0x3;                      /* add slot context + EP0 context */
    xhci_build_slot_ctx(&in[8], s->speed, s->port + 1, 1);
    xhci_build_ep_ctx(&in[16], EP_TYPE_CONTROL, mps, 0,
                      xhci_ring_dequeue(&s->ep0));

    if (xhci_cmd(c, (unsigned int)in, 0, 0, TRB_ADDRESS_DEV, s->slot_id) < 0)
        return -1;

    s->ep0_mps = mps;
    return 0;
}

/* Evaluate Context: update the EP0 max packet after the first descriptor. */
static int xhci_update_ep0_mps(xhci_ctrl_t* c, xhci_slot_t* s, int mps)
{
    unsigned int* in = c->in_ctx;
    usb_memset(in, 0, 33 * XHCI_CTX_SIZE);

    in[1] = 0x3;                      /* slot + EP0 */
    xhci_build_slot_ctx(&in[8], s->speed, s->port + 1, xhci_highest_dci(s));
    xhci_build_ep_ctx(&in[16], EP_TYPE_CONTROL, mps, 0,
                      xhci_ring_dequeue(&s->ep0));

    if (xhci_cmd(c, (unsigned int)in, 0, 0, TRB_EVALUATE_CTX, s->slot_id) < 0)
        return -1;

    s->ep0_mps = mps;
    return 0;
}

/* Configure Endpoint: add the HID interrupt IN endpoint to the slot. */
static int xhci_configure_intr(xhci_ctrl_t* c, xhci_slot_t* s,
                               usb_endpoint_t* ep)
{
    int ep_num = ep->address & 0x0F;
    int dci = ep_num * 2 + ((ep->direction == USB_DIR_IN) ? 1 : 0);

    unsigned int* in = c->in_ctx;
    usb_memset(in, 0, 33 * XHCI_CTX_SIZE);

    in[1] = (1U << 0) | (1U << (unsigned int)dci);   /* slot + this endpoint */
    xhci_build_slot_ctx(&in[8], s->speed, s->port + 1, dci);
    xhci_build_ep_ctx(&in[8 + dci * 8], EP_TYPE_INTR_IN, ep->max_packet,
                      ep->interval ? ep->interval : 8,
                      xhci_ring_dequeue(&s->intr));

    if (xhci_cmd(c, (unsigned int)in, 0, 0, TRB_CONFIGURE_EP, s->slot_id) < 0)
        return -1;

    s->intr_dci = dci;
    s->intr_configured = 1;
    return 0;
}

/* Wait for the transfer event belonging to a specific TRB. */
static int xhci_wait_transfer(xhci_ctrl_t* c, unsigned int trb, int timeout_ms)
{
    int start = timer_get_ticks();
    int target = timeout_ms / 10;
    if (target < 1) target = 1;

    while (1) {
        xhci_process_events(c);
        if (c->xfer_valid && c->xfer_trb == trb) {
            int ok = (c->xfer_code == TRB_COMP_SUCCESS) ? 1 : 0;
            c->xfer_valid = 0;
            return ok ? 0 : -1;
        }
        if ((timer_get_ticks() - start) >= target) return -2;
        for (volatile int i = 0; i < 200; i++);
    }
}

/* EP0 control transfer. xHCI owns the device address, so SET_ADDRESS is
 * swallowed: it is already handled by the Address Device command. */
static int xhci_control(usb_hci_t* hci, usb_device_t* dev,
                        const usb_setup_packet_t* setup, void* data, int len,
                        int dir)
{
    xhci_ctrl_t* c = (xhci_ctrl_t*)hci->priv;
    xhci_slot_t* s = xhci_slot_for_port(c, dev->port);
    if (!s || !setup) return -1;

    if (setup->bRequest == USB_REQ_SET_ADDRESS) return 0;

    /* Follow the max packet size the device reported in its descriptor. */
    if (dev->max_packet0 && (int)dev->max_packet0 != s->ep0_mps) {
        if (xhci_update_ep0_mps(c, s, (int)dev->max_packet0) < 0) return -1;
    }

    int dir_in = (dir == USB_DIR_IN);
    /* TRT (bits 17:16 of the Setup TRB): 0 = no data stage, 2 = OUT, 3 = IN.
     * Without it the controller drives the wrong protocol and the device
     * stalls the request. */
    unsigned int trt = (len > 0) ? (dir_in ? 3U : 2U) : 0U;
    xhci_trb_t* t;

    t = xhci_ring_push(&s->ep0);
    /* IDT=1 means the 8 setup bytes live directly in the TRB parameter
     * (d0/d1), not in a buffer the TRB points at. */
    t->d0 = (unsigned int)setup->bmRequestType |
            ((unsigned int)setup->bRequest << 8) |
            ((unsigned int)setup->wValue << 16);
    t->d1 = (unsigned int)setup->wIndex |
            ((unsigned int)setup->wLength << 16);
    t->d2 = 8;
    t->d3 = (t->d3 & TRB_CYCLE) | TRB_IDT | (trt << 16) |
            TRB_TYPE(TRB_SETUP);

    if (len > 0 && data) {
        t = xhci_ring_push(&s->ep0);
        t->d0 = (unsigned int)data;
        t->d1 = 0;
        t->d2 = (unsigned int)len;
        t->d3 = (t->d3 & TRB_CYCLE) | TRB_TYPE(TRB_DATA) |
                (dir_in ? TRB_DIR_IN : 0U);
    }

    t = xhci_ring_push(&s->ep0);
    t->d0 = 0;
    t->d1 = 0;
    t->d2 = 0;
    t->d3 = (t->d3 & TRB_CYCLE) | TRB_IOC | TRB_TYPE(TRB_STATUS) |
            (dir_in ? 0U : TRB_DIR_IN);

    unsigned int status_trb = (unsigned int)t & ~0xFU;

    c->xfer_valid = 0;
    xhci_doorbell(c, s->slot_id, 1);        /* DCI 1 = EP0 */

    return xhci_wait_transfer(c, status_trb, 1000);
}

/* HID interrupt IN: arm one normal TRB, then collect its report on a later
 * poll. Keeping the transfer posted means no report can be lost between
 * main-loop iterations. */
static int xhci_intr_in(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                        void* data, int len)
{
    xhci_ctrl_t* c = (xhci_ctrl_t*)hci->priv;
    xhci_slot_t* s = xhci_slot_for_port(c, dev->port);
    if (!s || !ep) return -1;

    if (!s->intr_configured) {
        if (xhci_configure_intr(c, s, ep) < 0) {
            log_printf("[XHCI] endpoint 0x%X configuration failed\n",
                       ep->address);
            return -1;
        }
        log_printf("[XHCI] port %d endpoint 0x%X configured\n",
                   s->port + 1, ep->address);
    }

    if (s->intr_done) {
        int got = s->intr_len - s->intr_residual;
        s->intr_done = 0;
        if (got < 0) got = 0;
        if (got > len) got = len;
        if (got > 0) usb_memcpy(data, s->intr_buf, (unsigned int)got);
        return got;
    }

    if (!s->intr_armed) {
        int mps = ep->max_packet > 0 ? ep->max_packet : 8;
        if (mps > 64) mps = 64;

        xhci_trb_t* t = xhci_ring_push(&s->intr);
        t->d0 = (unsigned int)s->intr_buf;
        t->d1 = 0;
        t->d2 = (unsigned int)mps;
        t->d3 = (t->d3 & TRB_CYCLE) | TRB_IOC | TRB_TYPE(TRB_NORMAL);

        s->intr_trb = (unsigned int)t & ~0xFU;
        s->intr_len = mps;
        s->intr_armed = 1;
        xhci_doorbell(c, s->slot_id, s->intr_dci);
    }

    return 0;
}

/* PORTSC speed code -> USB_SPEED_*; anything above high speed is skipped
 * because this driver only implements the USB 2.0 side of xHCI. */
static int xhci_speed_of(unsigned int sc)
{
    int code = (int)((sc & XHCI_PORT_SPEED_MSK) >> XHCI_PORT_SPEED_SH);
    if (code == 1) return USB_SPEED_FULL;
    if (code == 2) return USB_SPEED_LOW;
    if (code == 3) return USB_SPEED_HIGH;
    return -1;
}

static int xhci_port_reset(xhci_ctrl_t* c, int port)
{
    unsigned int sc = xhci_port_read(c, port);
    if (!(sc & XHCI_PORT_CCS)) return -1;

    /* Acknowledge stale change bits first (they are write-1-to-clear). */
    if (sc & XHCI_PORT_CHANGE) {
        xhci_port_write(c, port, sc | XHCI_PORT_CHANGE);
        sc = xhci_port_read(c, port);
    }

    if (!(sc & XHCI_PORT_PED)) {
        xhci_port_write(c, port, (sc & ~XHCI_PORT_CHANGE) | XHCI_PORT_PR);

        int start = timer_get_ticks();
        while (!(xhci_port_read(c, port) & XHCI_PORT_PRC)) {
            if ((timer_get_ticks() - start) >= 50) break;   /* ~500 ms */
            sleep_ms(10);
        }

        sc = xhci_port_read(c, port);
        xhci_port_write(c, port, sc);       /* clear PRC/CSC/... */
        sleep_ms(20);
        sc = xhci_port_read(c, port);
    }

    if (!(sc & XHCI_PORT_CCS)) return -1;
    return xhci_speed_of(sc);
}

static int xhci_hci_port_reset(usb_hci_t* hci, int port)
{
    return xhci_port_reset((xhci_ctrl_t*)hci->priv, port);
}

/* Enable Slot + Address Device for the device on this port. */
static int xhci_setup_device(xhci_ctrl_t* c, int port, int speed)
{
    if (xhci_slot_for_port(c, port)) return 0;

    xhci_slot_t* s = 0;
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (!c->slots[i].used) {
            s = &c->slots[i];
            break;
        }
    }
    if (!s) return -1;

    usb_memset(s, 0, sizeof(*s));

    void* ep0_page = pmm_alloc_pages_aligned(1, 1);
    void* intr_page = pmm_alloc_pages_aligned(1, 1);
    void* dev_ctx_page = pmm_alloc_pages_aligned(1, 1);
    void* intr_buf_page = pmm_alloc_pages_aligned(1, 1);
    if (!ep0_page || !intr_page || !dev_ctx_page || !intr_buf_page) {
        log_printf("[XHCI] out of memory for slot structures\n");
        return -1;
    }

    usb_memset(ep0_page, 0, 4096);
    usb_memset(intr_page, 0, 4096);
    usb_memset(dev_ctx_page, 0, 4096);
    usb_memset(intr_buf_page, 0, 4096);

    xhci_ring_init(&s->ep0, ep0_page, XHCI_RING_TRBS);
    xhci_ring_init(&s->intr, intr_page, XHCI_RING_TRBS);
    s->dev_ctx = (unsigned char*)dev_ctx_page;
    s->intr_buf = (unsigned char*)intr_buf_page;

    s->port = port;
    s->speed = speed;
    s->used = 1;

    int slot = xhci_cmd(c, 0, 0, 0, TRB_ENABLE_SLOT, 0);
    if (slot <= 0) {
        log_printf("[XHCI] port %d: enable slot failed\n", port + 1);
        s->used = 0;
        return -1;
    }

    s->slot_id = slot;
    c->dcbaa[slot * 2] = (unsigned int)dev_ctx_page;
    c->dcbaa[slot * 2 + 1] = 0;

    int mps = (speed == USB_SPEED_HIGH) ? 64 : 8;
    if (xhci_address_device(c, s, mps) < 0) {
        log_printf("[XHCI] port %d: address device failed\n", port + 1);
        s->used = 0;
        return -1;
    }

    log_printf("[XHCI] port %d: slot %d, %s speed, EP0 mps %d\n", port + 1,
               slot, speed == USB_SPEED_HIGH ? "high" :
               (speed == USB_SPEED_LOW ? "low" : "full"), mps);
    return 0;
}

/* BIOS legacy USB support lives in an xHCI extended capability. Without this
 * handoff the firmware SMM handler keeps driving the controller behind our
 * back and the devices get torn down as soon as we reset it. */
static void xhci_legacy_handoff(xhci_ctrl_t* c)
{
    unsigned int hcc1 = xhci_cap_read(c, XHCI_HCCPARAMS1);
    unsigned int xecp = (hcc1 >> 16) & 0xFFFFU;
    int guard = 0;

    while (xecp && guard++ < 64) {
        volatile unsigned int* ext =
            (volatile unsigned int*)(c->bar + (xecp << 2));
        unsigned int val = *ext;
        unsigned int id = val & 0xFFU;
        unsigned int next = (val >> 8) & 0xFFU;

        if (id == 0x01U) {                      /* USB Legacy Support */
            if (val & 0x00010000U) {            /* BIOS still owns it */
                *ext = val | 0x01000000U;       /* claim OS ownership */
                for (int i = 0; i < 100; i++) {
                    if (!(*ext & 0x00010000U)) break;
                    sleep_ms(10);
                }
            }
            *(ext + 1) = 0;                     /* USBLEGCTLSTS: no SMIs */
            log_printf("[XHCI] legacy USB support taken over from firmware\n");
            return;
        }

        if (!next) break;
        xecp += next;
    }
}

static void xhci_scan_ports(xhci_ctrl_t* c)
{
    unsigned int hcc1 = xhci_cap_read(c, XHCI_HCCPARAMS1);
    int ppc = (int)((hcc1 >> 3) & 1U);
    int found = 0;

    for (int port = 0; port < c->max_ports; port++) {
        unsigned int sc = xhci_port_read(c, port);

        if (ppc && !(sc & XHCI_PORT_PP)) {
            xhci_port_write(c, port, (sc & ~XHCI_PORT_CHANGE) | XHCI_PORT_PP);
            sleep_ms(20);
            sc = xhci_port_read(c, port);
        }
        if (!(sc & XHCI_PORT_CCS)) continue;

        int speed = xhci_port_reset(c, port);
        if (speed < 0) continue;        /* SuperSpeed port or failed reset */

        if (xhci_setup_device(c, port, speed) < 0) continue;
        if (usb_enumerate(&c->hci, port, speed) < 0) {
            log_printf("[XHCI] port %d: enumeration failed\n", port + 1);
            continue;
        }
        found++;
    }

    log_printf("[XHCI] %d USB 2.0 device(s) enumerated\n", found);
}

static void xhci_hci_poll(usb_hci_t* hci)
{
    xhci_process_events((xhci_ctrl_t*)hci->priv);
}

void xhci_poll(void)
{
    for (int i = 0; i < g_xhci_count; i++) xhci_process_events(&g_xhci[i]);
}

int xhci_probe(const pci_device_t* dev)
{
    if (g_xhci_count >= 2) return -1;
    if (!dev || !dev->bar_valid[0] || dev->bar_is_io[0]) return -1;

    xhci_ctrl_t* c = &g_xhci[g_xhci_count];
    usb_memset(c, 0, sizeof(*c));

    c->bar = dev->bar[0] & ~0xFU;
    pci_enable_memory(dev);
    pci_enable_bus_master(dev);

    unsigned char cap_len = (unsigned char)xhci_cap_read(c, XHCI_CAPLENGTH);
    /* CAPLENGTH occupies byte 0 and HCIVERSION bytes 2-3 of the same dword. */
    unsigned short version =
        (unsigned short)((xhci_cap_read(c, XHCI_CAPLENGTH) >> 16) & 0xFFFFU);

    unsigned int hcs1 = xhci_cap_read(c, XHCI_HCSPARAMS1);
    unsigned int hcs2 = xhci_cap_read(c, XHCI_HCSPARAMS2);
    unsigned int hcc1 = xhci_cap_read(c, XHCI_HCCPARAMS1);

    /* CSZ=1 means 64-byte contexts; this driver only knows the 32-byte form. */
    if (hcc1 & 0x00000004U) {
        log_printf("[XHCI] 64-byte contexts unsupported (bus %u slot %u)\n",
                   dev->bus, dev->slot);
        return -1;
    }

    c->cap = (volatile unsigned int*)c->bar;
    c->op = (volatile unsigned int*)(c->bar + cap_len);
    c->dboff = xhci_cap_read(c, XHCI_DBOFF) & ~0x3U;
    c->rt = (volatile unsigned int*)(c->bar +
             (xhci_cap_read(c, XHCI_RTSOFF) & ~0x1FU));

    c->max_slots = (int)(hcs1 & 0xFFU);
    c->max_intrs = (int)((hcs1 >> 8) & 0x7FFU);
    c->max_ports = (int)((hcs1 >> 24) & 0xFFU);
    if (c->max_slots > XHCI_MAX_SLOTS) c->max_slots = XHCI_MAX_SLOTS;
    if (c->max_ports > XHCI_MAX_PORTS) c->max_ports = XHCI_MAX_PORTS;
    if (c->max_slots <= 0 || c->max_ports <= 0) {
        log_printf("[XHCI] unusable controller (slots=%d ports=%d)\n",
                   c->max_slots, c->max_ports);
        return -1;
    }

    log_printf("[XHCI] v%X.%X at MMIO 0x%X: %d slots, %d ports\n",
               (version >> 8) & 0xFF, version & 0xFF, c->bar,
               c->max_slots, c->max_ports);

    /* Ask firmware to let go of the controller before resetting it. */
    xhci_legacy_handoff(c);

    xhci_op_write(c, XHCI_USBCMD, 0);
    int start = timer_get_ticks();
    while (!(xhci_op_read(c, XHCI_USBSTS) & XHCI_STS_HCH)) {
        if ((timer_get_ticks() - start) >= 30) break;
        sleep_ms(10);
    }

    xhci_op_write(c, XHCI_USBCMD, XHCI_CMD_HCRST);
    start = timer_get_ticks();
    while (xhci_op_read(c, XHCI_USBCMD) & XHCI_CMD_HCRST) {
        if ((timer_get_ticks() - start) >= 100) break;
        sleep_ms(10);
    }
    start = timer_get_ticks();
    while (xhci_op_read(c, XHCI_USBSTS) & XHCI_STS_CNR) {
        if ((timer_get_ticks() - start) >= 100) break;
        sleep_ms(10);
    }

    xhci_op_write(c, XHCI_PAGESIZE, 1);            /* 4 KB pages */

    void* dcbaa_page = pmm_alloc_pages_aligned(1, 1);
    void* cmd_page = pmm_alloc_pages_aligned(1, 1);
    void* evt_page = pmm_alloc_pages_aligned(1, 1);
    void* erst_page = pmm_alloc_pages_aligned(1, 1);
    void* ctx_page = pmm_alloc_pages_aligned(1, 1);
    if (!dcbaa_page || !cmd_page || !evt_page || !erst_page || !ctx_page) {
        log_printf("[XHCI] out of memory for controller structures\n");
        return -1;
    }

    usb_memset(dcbaa_page, 0, 4096);
    usb_memset(evt_page, 0, 4096);
    usb_memset(erst_page, 0, 4096);
    usb_memset(ctx_page, 0, 4096);

    c->dcbaa = (unsigned int*)dcbaa_page;
    c->evt = (xhci_trb_t*)evt_page;
    c->evt_phys = (unsigned int)evt_page;
    c->evt_idx = 0;
    c->evt_cycle = 1;
    c->erst = (unsigned int*)erst_page;
    c->in_ctx = (unsigned int*)ctx_page;

    /* Scratchpad buffers are required by Intel PCH controllers. */
    int num_sp = (int)(((hcs2 >> 16) & 0x3E0U) | ((hcs2 >> 27) & 0x1FU));
    if (num_sp > 0) {
        unsigned int* sp_array =
            (unsigned int*)pmm_alloc_pages_aligned(1, 1);
        if (!sp_array) {
            log_printf("[XHCI] scratchpad array allocation failed\n");
            return -1;
        }
        usb_memset(sp_array, 0, 4096);

        int allocated = 0;
        for (int i = 0; i < num_sp && i < 512; i++) {
            void* buf = pmm_alloc_page();
            if (!buf) break;
            usb_memset(buf, 0, 4096);
            sp_array[i * 2] = (unsigned int)buf;
            sp_array[i * 2 + 1] = 0;
            allocated++;
        }
        c->dcbaa[0] = (unsigned int)sp_array;
        c->dcbaa[1] = 0;
        log_printf("[XHCI] %d of %d scratchpad buffer(s) allocated\n",
                   allocated, num_sp);
        if (allocated < num_sp) {
            /* The controller dereferences every scratchpad pointer; starting it
             * with a short array makes it raise a host controller error and
             * take all ports down. Leave it halted instead. */
            log_printf("[XHCI] incomplete scratchpad set, refusing to start\n");
            return -1;
        }
    }

    xhci_ring_init(&c->cmd, cmd_page, XHCI_RING_TRBS);

    /* Event Ring Segment Table entry: {base_low, base_high, size_in_trbs, 0}. */
    c->erst[0] = c->evt_phys;
    c->erst[1] = 0;
    c->erst[2] = XHCI_EVENT_TRBS;
    c->erst[3] = 0;

    /* Every 64-bit register is programmed low dword first: the controller
     * latches the full address (command ring base, ERST base) when the high
     * dword arrives. */
    xhci_op_write(c, XHCI_DCBAAP, (unsigned int)c->dcbaa);
    xhci_op_write(c, XHCI_DCBAAP + 4, 0);
    xhci_op_write(c, XHCI_CRCR, (c->cmd.phys & ~0x3FU) | 1U);
    xhci_op_write(c, XHCI_CRCR + 4, 0);
    xhci_op_write(c, XHCI_CONFIG, (unsigned int)c->max_slots);

    /* Interrupter 0: one event segment, interrupts deliberately left off. */
    xhci_rt_write(c, XHCI_IMAN, 0);
    xhci_rt_write(c, XHCI_IMOD, 0);
    xhci_rt_write(c, XHCI_ERSTSZ, 1);
    xhci_rt_write(c, XHCI_ERSTBA, (unsigned int)c->erst);
    xhci_rt_write(c, XHCI_ERSTBA + 4, 0);      /* latches the ERST base */
    xhci_rt_write(c, XHCI_ERDP, c->evt_phys | 0x8U);
    xhci_rt_write(c, XHCI_ERDP + 4, 0);

    unsigned int cmd = XHCI_CMD_RS;
    if (version > 0x0100) cmd |= XHCI_CMD_EWE;
    xhci_op_write(c, XHCI_USBCMD, cmd);
    sleep_ms(100);          /* let the USB 2.0 ports settle in RxDetect */

    if (xhci_op_read(c, XHCI_USBSTS) & (XHCI_STS_HCH | XHCI_STS_HCE)) {
        log_printf("[XHCI] controller failed to start (USBSTS=0x%X)\n",
                   xhci_op_read(c, XHCI_USBSTS));
        return -1;
    }

    c->hci.name = "xHCI";
    c->hci.priv = c;
    c->hci.control = xhci_control;
    c->hci.intr_in = xhci_intr_in;
    c->hci.bulk = 0;
    c->hci.port_reset = xhci_hci_port_reset;
    c->hci.poll = xhci_hci_poll;

    usb_register_hci(&c->hci);
    g_xhci_count++;

    xhci_scan_ports(c);
    return 0;
}







