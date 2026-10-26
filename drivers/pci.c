#include "pci.h"
#include "port.h"
#include "log.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

static pci_device_t g_devices[PCI_MAX_DEVICES];
static int g_device_count;
static int g_scanned;

unsigned int pci_config_read(unsigned char bus, unsigned char slot,
                             unsigned char function, unsigned char offset)
{
    return pci_read_config(bus, slot, function, offset);
}

void pci_config_write(unsigned char bus, unsigned char slot,
                      unsigned char function, unsigned char offset,
                      unsigned int value)
{
    pci_write_config(bus, slot, function, offset, value);
}

static void pci_decode_bar(pci_device_t* dev, int index)
{
    unsigned int raw = pci_config_read(dev->bus, dev->slot, dev->function,
                                       0x10 + index * 4);
    dev->bar[index] = 0;
    dev->bar_high[index] = 0;
    dev->bar_is_io[index] = 0;
    dev->bar_is_64bit[index] = 0;
    dev->bar_valid[index] = 0;

    if (raw == 0 || raw == 0xFFFFFFFFU) return;

    if (raw & 1U) {
        dev->bar[index] = raw & ~3U;
        dev->bar_is_io[index] = 1;
        dev->bar_valid[index] = dev->bar[index] != 0;
        return;
    }

    unsigned int type = (raw >> 1) & 3U;
    dev->bar[index] = raw & ~0xFU;
    if (type == 2U && index + 1 < PCI_BAR_COUNT) {
        dev->bar_is_64bit[index] = 1;
        dev->bar_high[index] =
            pci_config_read(dev->bus, dev->slot, dev->function,
                            0x14 + index * 4);
        dev->bar_valid[index] =
            dev->bar[index] != 0 && dev->bar_high[index] == 0;
    } else if (type != 1U) {
        dev->bar_valid[index] = dev->bar[index] != 0;
    }
}

static void pci_add_function(unsigned char bus, unsigned char slot,
                             unsigned char function)
{
    if (g_device_count >= PCI_MAX_DEVICES) return;

    unsigned int id = pci_config_read(bus, slot, function, 0);
    if (id == 0 || id == 0xFFFFFFFFU) return;

    pci_device_t* dev = &g_devices[g_device_count];
    for (unsigned int i = 0; i < sizeof(*dev); i++) {
        ((unsigned char*)dev)[i] = 0;
    }

    dev->bus = bus;
    dev->slot = slot;
    dev->function = function;
    dev->vendor = (unsigned short)(id & 0xFFFFU);
    dev->device = (unsigned short)(id >> 16);

    unsigned int class_rev = pci_config_read(bus, slot, function, 0x08);
    dev->revision = (unsigned char)(class_rev & 0xFFU);
    dev->prog_if = (unsigned char)((class_rev >> 8) & 0xFFU);
    dev->subclass = (unsigned char)((class_rev >> 16) & 0xFFU);
    dev->class_code = (unsigned char)((class_rev >> 24) & 0xFFU);

    unsigned int header = pci_config_read(bus, slot, function, 0x0C);
    dev->header_type = (unsigned char)((header >> 16) & 0xFFU);

    unsigned int irq = pci_config_read(bus, slot, function, 0x3C);
    dev->irq_line = (unsigned char)(irq & 0xFFU);
    dev->irq_pin = (unsigned char)((irq >> 8) & 0xFFU);

    if ((dev->header_type & 0x7FU) == 0) {
        unsigned int subsystem =
            pci_config_read(bus, slot, function, 0x2C);
        dev->subsystem_vendor = (unsigned short)(subsystem & 0xFFFFU);
        dev->subsystem_device = (unsigned short)(subsystem >> 16);
    }

    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        pci_decode_bar(dev, i);
        if (dev->bar_is_64bit[i]) i++;
    }

    g_device_count++;
}

void pci_scan_all(void)
{
    if (g_scanned) return;

    g_device_count = 0;
    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int slot = 0; slot < 32; slot++) {
            unsigned int id = pci_config_read((unsigned char)bus,
                                              (unsigned char)slot, 0, 0);
            if (id == 0 || id == 0xFFFFFFFFU) continue;

            unsigned int header =
                pci_config_read((unsigned char)bus, (unsigned char)slot, 0,
                                0x0C);
            int functions = (header & 0x00800000U) ? 8 : 1;
            for (int function = 0; function < functions; function++) {
                pci_add_function((unsigned char)bus, (unsigned char)slot,
                                 (unsigned char)function);
            }
        }
    }

    g_scanned = 1;
    log_printf("[PCI ] enumerated %u device(s), cache limit=%u\n",
               (unsigned int)g_device_count, (unsigned int)PCI_MAX_DEVICES);
}

int pci_has_scanned(void)
{
    return g_scanned;
}

int pci_get_device_count(void)
{
    if (!g_scanned) pci_scan_all();
    return g_device_count;
}

const pci_device_t* pci_get_device(int index)
{
    if (!g_scanned) pci_scan_all();
    if (index < 0 || index >= g_device_count) return NULL;
    return &g_devices[index];
}

const pci_device_t* pci_find_by_id(unsigned short vendor,
                                   unsigned short device)
{
    if (!g_scanned) pci_scan_all();
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].vendor == vendor && g_devices[i].device == device) {
            return &g_devices[i];
        }
    }
    return NULL;
}

const pci_device_t* pci_find_class(unsigned char class_code,
                                   unsigned char subclass)
{
    if (!g_scanned) pci_scan_all();
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].class_code == class_code &&
            g_devices[i].subclass == subclass) {
            return &g_devices[i];
        }
    }
    return NULL;
}

static unsigned int pci_update_command(const pci_device_t* dev,
                                       unsigned int set_bits)
{
    if (!dev) return 0;
    unsigned int command =
        pci_config_read(dev->bus, dev->slot, dev->function, 0x04);
    unsigned int updated = command | set_bits;
    if (updated != command) {
        pci_config_write(dev->bus, dev->slot, dev->function, 0x04, updated);
    }
    return updated;
}

unsigned int pci_enable_io(const pci_device_t* dev)
{
    return pci_update_command(dev, 0x0001U);
}

unsigned int pci_enable_memory(const pci_device_t* dev)
{
    return pci_update_command(dev, 0x0002U);
}

unsigned int pci_enable_bus_master(const pci_device_t* dev)
{
    return pci_update_command(dev, 0x0004U);
}

unsigned int pci_get_bar_size(const pci_device_t* dev, int bar)
{
    if (!dev || bar < 0 || bar >= PCI_BAR_COUNT || !dev->bar_valid[bar]) {
        return 0;
    }

    unsigned char command_offset = 0x04;
    unsigned char bar_offset = (unsigned char)(0x10 + bar * 4);
    unsigned int command =
        pci_config_read(dev->bus, dev->slot, dev->function, command_offset);
    unsigned int original = pci_config_read(dev->bus, dev->slot, dev->function,
                                            bar_offset);
    unsigned int original_high = 0;

    pci_config_write(dev->bus, dev->slot, dev->function, command_offset,
                     command & ~3U);
    pci_config_write(dev->bus, dev->slot, dev->function, bar_offset,
                     0xFFFFFFFFU);
    unsigned int mask = pci_config_read(dev->bus, dev->slot, dev->function,
                                        bar_offset);

    if (dev->bar_is_64bit[bar]) {
        unsigned char high_offset = (unsigned char)(bar_offset + 4);
        original_high = pci_config_read(dev->bus, dev->slot, dev->function,
                                        high_offset);
        pci_config_write(dev->bus, dev->slot, dev->function, high_offset,
                         0xFFFFFFFFU);
        unsigned int mask_high =
            pci_config_read(dev->bus, dev->slot, dev->function, high_offset);
        pci_config_write(dev->bus, dev->slot, dev->function, high_offset,
                         original_high);
        if (mask_high != 0xFFFFFFFFU) mask = 0xFFFFFFFFU;
    }

    pci_config_write(dev->bus, dev->slot, dev->function, bar_offset, original);
    pci_config_write(dev->bus, dev->slot, dev->function, command_offset,
                     command);

    if (dev->bar_is_io[bar]) {
        mask &= ~3U;
    } else {
        mask &= ~0xFU;
    }
    if (!mask || mask == 0xFFFFFFFCU || mask == 0xFFFFFFF0U) return 0;
    return (~mask) + 1U;
}

const char* pci_class_name(unsigned char class_code, unsigned char subclass)
{
    if (class_code == 0x03) return "Display Controller";
    if (class_code == 0x04 && subclass == 0x03) return "HD Audio";
    if (class_code == 0x04) return "Multimedia";
    if (class_code == 0x01 && subclass == 0x01) return "IDE Controller";
    if (class_code == 0x01 && subclass == 0x06) return "SATA/AHCI";
    if (class_code == 0x02) return "Network Controller";
    if (class_code == 0x06) return "Bridge";
    if (class_code == 0x0C && subclass == 0x03) return "USB Controller";
    return "Other";
}
