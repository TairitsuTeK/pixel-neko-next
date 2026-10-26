#ifndef USB_CORE_H
#define USB_CORE_H

/*
 * usb_core.h - USB 2.0 core layer (device model + HCI abstraction)
 *
 * Pixel Neko Next. Provides shared USB descriptor/request definitions,
 * a small host-controller-interface (HCI) abstraction and the public
 * enumeration/transfer API used by the EHCI / UHCI drivers and HID class
 * driver.
 */

#include "pci.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

/* ---- USB descriptor types ---- */
#define USB_DT_DEVICE     0x01
#define USB_DT_CONFIG     0x02
#define USB_DT_STRING     0x03
#define USB_DT_INTERFACE  0x04
#define USB_DT_ENDPOINT   0x05
#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22

/* ---- Standard device requests ---- */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B

/* ---- Request type bit fields ---- */
#define USB_DIR_OUT          0x00
#define USB_DIR_IN           0x80
#define USB_RT_STANDARD      0x00
#define USB_RT_CLASS         0x20
#define USB_RT_VENDOR        0x40
#define USB_RT_DEVICE        0x00
#define USB_RT_INTERFACE     0x01
#define USB_RT_ENDPOINT      0x02

/* ---- Endpoint attributes ---- */
#define USB_EP_TYPE_CONTROL     0x00
#define USB_EP_TYPE_ISOCHRONOUS 0x01
#define USB_EP_TYPE_BULK        0x02
#define USB_EP_TYPE_INTERRUPT   0x03

/* ---- Device speeds ---- */
#define USB_SPEED_LOW   1
#define USB_SPEED_FULL  2
#define USB_SPEED_HIGH  3

#define USB_MAX_ENDPOINTS 8

/* ---- Standard descriptors (packed, wire format) ---- */
typedef struct {
    unsigned char  bmRequestType;
    unsigned char  bRequest;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short bcdUSB;
    unsigned char  bDeviceClass;
    unsigned char  bDeviceSubClass;
    unsigned char  bDeviceProtocol;
    unsigned char  bMaxPacketSize0;
    unsigned short idVendor;
    unsigned short idProduct;
    unsigned short bcdDevice;
    unsigned char  iManufacturer;
    unsigned char  iProduct;
    unsigned char  iSerialNumber;
    unsigned char  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short wTotalLength;
    unsigned char  bNumInterfaces;
    unsigned char  bConfigurationValue;
    unsigned char  iConfiguration;
    unsigned char  bmAttributes;
    unsigned char  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned char  bInterfaceNumber;
    unsigned char  bAlternateSetting;
    unsigned char  bNumEndpoints;
    unsigned char  bInterfaceClass;
    unsigned char  bInterfaceSubClass;
    unsigned char  bInterfaceProtocol;
    unsigned char  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned char  bEndpointAddress;
    unsigned char  bmAttributes;
    unsigned short wMaxPacketSize;
    unsigned char  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short bcdHID;
    unsigned char  bCountryCode;
    unsigned char  bNumDescriptors;
    unsigned char  bReportType;
    unsigned short wReportLength;
} __attribute__((packed)) usb_hid_desc_t;

/* ---- In-memory device model ---- */
typedef struct usb_hci usb_hci_t;

typedef struct {
    unsigned char  address;       /* endpoint address incl. direction bit */
    unsigned char  type;          /* USB_EP_TYPE_* */
    unsigned char  direction;     /* USB_DIR_IN / USB_DIR_OUT */
    unsigned short max_packet;
    unsigned char  interval;      /* polling interval */
    unsigned char  toggle;        /* data toggle bit */
    void*          hcd_priv;      /* host-controller private data */
} usb_endpoint_t;

typedef struct {
    usb_hci_t* hci;
    unsigned char address;        /* assigned USB device address */
    unsigned char speed;          /* USB_SPEED_* */
    unsigned char port;           /* host controller port number */
    unsigned short max_packet0;   /* EP0 max packet size */
    unsigned short vendor;
    unsigned short product;
    unsigned char  class_code;
    unsigned char  subclass;
    unsigned char  protocol;

    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    int endpoint_count;

    unsigned char config_value;
    unsigned char interface_number;    /* first interface number */
    unsigned char interface_protocol;  /* HID: 1=keyboard, 2=mouse */
} usb_device_t;

/*
 * Host Controller Interface abstraction. Each HCD (EHCI / UHCI) provides one
 * of these. The HID class driver and the core enumerator only talk through
 * this table, keeping the transport and controller details behind it.
 */
struct usb_hci {
    const char* name;
    void*       priv;

    /* Data-stage direction is encoded in dir (USB_DIR_IN / USB_DIR_OUT).
     * Returns 0 on success, negative on failure. */
    int  (*control)(usb_hci_t* hci, usb_device_t* dev,
                    const usb_setup_packet_t* setup, void* data, int len,
                    int dir);

    /* Interrupt transfer (HID reports are always IN on the IN endpoint). */
    int  (*intr_in)(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                    void* data, int len);

    /* Bulk transfer (kept for future mass-storage support). */
    int  (*bulk)(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                 void* data, int len, int dir);

    /* Perform a port reset + speed detection for the given port.
     * Returns USB_SPEED_LOW / USB_SPEED_FULL / USB_SPEED_HIGH on success,
     * or a negative value on failure. */
    int  (*port_reset)(usb_hci_t* hci, int port);

    /* Periodic housekeeping (reap finished HID transfers). */
    void (*poll)(usb_hci_t* hci);
};

/* ---- Public API ---- */
void usb_init(void);
void usb_poll(void);

/* Called by each HCD once it is initialized. */
void usb_register_hci(usb_hci_t* hci);

/* Called by an HCD after it detects a device on a port (post reset). */
int  usb_enumerate(usb_hci_t* hci, int port, int speed);

int  usb_control_msg(usb_device_t* dev, int dir, unsigned char request,
                     unsigned short value, unsigned short index,
                     void* data, int len);

/* Helper: fill a standard GET_DESCRIPTOR setup packet. */
void usb_build_setup(usb_setup_packet_t* s, int dir, unsigned char request,
                     unsigned short value, unsigned short index,
                     unsigned short length);

/* Simple byte helpers (no libc in this kernel). */
void usb_memset(void* dst, unsigned char c, unsigned int n);
void usb_memcpy(void* dst, const void* src, unsigned int n);

#endif /* USB_CORE_H */
