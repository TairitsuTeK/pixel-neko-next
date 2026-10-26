#ifndef PCI_H
#define PCI_H

#define PCI_MAX_DEVICES 256
#define PCI_BAR_COUNT 6

typedef struct {
    unsigned char bus;
    unsigned char slot;
    unsigned char function;
    unsigned short vendor;
    unsigned short device;
    unsigned short subsystem_vendor;
    unsigned short subsystem_device;
    unsigned char revision;
    unsigned char prog_if;
    unsigned char subclass;
    unsigned char class_code;
    unsigned char header_type;
    unsigned char irq_line;
    unsigned char irq_pin;
    unsigned int bar[PCI_BAR_COUNT];
    unsigned int bar_high[PCI_BAR_COUNT];
    unsigned char bar_is_io[PCI_BAR_COUNT];
    unsigned char bar_is_64bit[PCI_BAR_COUNT];
    unsigned char bar_valid[PCI_BAR_COUNT];
} pci_device_t;

void pci_scan_all(void);
int pci_has_scanned(void);
int pci_get_device_count(void);
const pci_device_t* pci_get_device(int index);
const pci_device_t* pci_find_by_id(unsigned short vendor, unsigned short device);
const pci_device_t* pci_find_class(unsigned char class_code, unsigned char subclass);
unsigned int pci_config_read(unsigned char bus, unsigned char slot,
                             unsigned char function, unsigned char offset);
void pci_config_write(unsigned char bus, unsigned char slot,
                      unsigned char function, unsigned char offset,
                      unsigned int value);
unsigned int pci_enable_io(const pci_device_t* dev);
unsigned int pci_enable_memory(const pci_device_t* dev);
unsigned int pci_enable_bus_master(const pci_device_t* dev);
unsigned int pci_get_bar_size(const pci_device_t* dev, int bar);
const char* pci_class_name(unsigned char class_code, unsigned char subclass);

#endif
