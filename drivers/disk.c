#include "disk.h"
#include "port.h"

#define ATA_CMD_READ        0x20
#define ATA_CMD_WRITE       0x30
#define ATA_CMD_READ_EXT    0x24
#define ATA_CMD_WRITE_EXT   0x34
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_TIMEOUT  1000000

static int grub_mode = 1;
disk_drive_t g_drives[MAX_DRIVES];

void disk_set_grub_mode(int mode) { grub_mode = mode; }
int disk_get_grub_mode(void) { return grub_mode; }

static int ata_wait_ready(unsigned int port)
{
    int timeout = ATA_TIMEOUT;
    while (timeout > 0) {
        unsigned char status = inb(port + 7);
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRDY) != 0) return 0;
        timeout--;
    }
    return -1;
}

static int ata_wait_drq(unsigned int port)
{
    int timeout = ATA_TIMEOUT;
    while (timeout > 0) {
        unsigned char status = inb(port + 7);
        if ((status & ATA_SR_BSY) == 0) {
            if (status & ATA_SR_ERR) return -2;
            if (status & ATA_SR_DRQ) return 0;
        }
        timeout--;
    }
    return -1;
}

static void ata_select_drive(unsigned int port, int drive_idx, unsigned int lba)
{
    outb(port + 6, 0xE0 | (drive_idx << 4) | ((lba >> 24) & 0x0F));
    inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);
}

typedef struct {
    unsigned int base;
    unsigned int ctrl;
} ide_channel_t;

int disk_detect_all(void)
{
    for (int i = 0; i < MAX_DRIVES; i++) {
        g_drives[i].present = 0;
        g_drives[i].is_atapi = 0;
        g_drives[i].base_port = 0;
        g_drives[i].ctrl_port = 0;
        g_drives[i].drive_idx = 0;
        g_drives[i].lba_offset = 0;
        g_drives[i].total_sectors = 0;
    }

    #define MAX_IDE_CHANNELS 8
    ide_channel_t channels[MAX_IDE_CHANNELS];
    int num_channels = 0;

    // Helper to add channel without duplicates
    #define ADD_CHANNEL(b, c) do { \
        unsigned int _b = (b); \
        unsigned int _c = (c); \
        if (_b >= 0x100 && _b <= 0xFFF0 && num_channels < MAX_IDE_CHANNELS) { \
            int _dup = 0; \
            for (int _k = 0; _k < num_channels; _k++) { \
                if (channels[_k].base == _b) { _dup = 1; break; } \
            } \
            if (!_dup) { \
                channels[num_channels].base = _b; \
                channels[num_channels].ctrl = _c ? _c : (_b + 0x206); \
                num_channels++; \
            } \
        } \
    } while (0)

    // 1. Standard legacy IDE channels
    ADD_CHANNEL(0x1F0, 0x3F6);
    ADD_CHANNEL(0x170, 0x376);

    // 2. Scan PCI bus for PCI IDE & SATA Native Controllers (Intel H55, ICH, etc.)
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                unsigned int id = pci_read_config(bus, dev, func, 0);
                if (id == 0xFFFFFFFF || id == 0) {
                    if (func == 0) break; // Slot empty
                    continue;
                }

                unsigned int class_rev = pci_read_config(bus, dev, func, 0x08);
                unsigned char pci_class = (class_rev >> 24) & 0xFF;

                // Mass storage controllers: IDE, SATA in IDE mode, RAID in IDE mode
                if (pci_class == 0x01) {
                    // Enable I/O Space (bit 0), Memory Space (bit 1), Bus Master (bit 2)
                    unsigned int cmd = pci_read_config(bus, dev, func, 0x04);
                    pci_write_config(bus, dev, func, 0x04, cmd | 0x07);

                    // BAR0 & BAR1: Primary Channel
                    unsigned int bar0 = pci_read_config(bus, dev, func, 0x10);
                    unsigned int bar1 = pci_read_config(bus, dev, func, 0x14);
                    if (bar0 & 1) {
                        unsigned int b0 = bar0 & 0xFFFC;
                        unsigned int c0 = (bar1 & 1) ? (bar1 & 0xFFFC) : (b0 + 0x206);
                        ADD_CHANNEL(b0, c0);
                    }

                    // BAR2 & BAR3: Secondary Channel
                    unsigned int bar2 = pci_read_config(bus, dev, func, 0x18);
                    unsigned int bar3 = pci_read_config(bus, dev, func, 0x1C);
                    if (bar2 & 1) {
                        unsigned int b2 = bar2 & 0xFFFC;
                        unsigned int c2 = (bar3 & 1) ? (bar3 & 0xFFFC) : (b2 + 0x206);
                        ADD_CHANNEL(b2, c2);
                    }
                }
            }
        }
    }

    int drive_count = 0;

    for (int p = 0; p < num_channels && drive_count < MAX_DRIVES; p++) {
        unsigned int port = channels[p].base;
        unsigned int ctrl = channels[p].ctrl;

        for (int d = 0; d < 2 && drive_count < MAX_DRIVES; d++) {
            // Select drive (0 = Master, 1 = Slave)
            outb(port + 6, 0xA0 | (d << 4));
            inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);

            unsigned char st = inb(port + 7);
            if (st == 0xFF) continue; // Floating bus -> no drive

            if (st == 0x00) {
                int wait_cnt = 50000;
                while (wait_cnt > 0 && st == 0x00) {
                    st = inb(port + 7);
                    wait_cnt--;
                }
                if (st == 0x00 || st == 0xFF) continue;
            }

            // Wait for BSY to clear
            int timeout = 100000;
            while (timeout > 0) {
                st = inb(port + 7);
                if ((st & ATA_SR_BSY) == 0) break;
                timeout--;
            }
            if (timeout == 0) continue;

            // Prepare Task File registers
            outb(port + 2, 0); // Sector Count
            outb(port + 3, 0); // LBA low
            outb(port + 4, 0); // LBA mid
            outb(port + 5, 0); // LBA high
            outb(port + 7, ATA_CMD_IDENTIFY);
            inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);

            // Wait for BSY to clear
            timeout = 100000;
            while (timeout > 0) {
                st = inb(port + 7);
                if ((st & ATA_SR_BSY) == 0) break;
                timeout--;
            }
            if (timeout == 0) continue;

            unsigned char cl = inb(port + 4);
            unsigned char ch = inb(port + 5);

            // 1. Check for ATAPI CD-ROM / Optical device
            if ((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96)) {
                outb(port + 7, ATA_CMD_IDENTIFY_PACKET);
                inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);

                // ATAPI device must assert DRQ for packet identify data
                if (ata_wait_drq(port) == 0) {
                    unsigned short buf[256];
                    for (int i = 0; i < 256; i++) buf[i] = inw(port + 0);

                    g_drives[drive_count].present = 1;
                    g_drives[drive_count].is_atapi = 1;
                    g_drives[drive_count].base_port = port;
                    g_drives[drive_count].ctrl_port = ctrl;
                    g_drives[drive_count].drive_idx = d;
                    g_drives[drive_count].lba_offset = 0;
                    g_drives[drive_count].total_sectors = 0;
                    drive_count++;
                }
                continue;
            }

            // 2. Standard ATA Hard Disk
            if (st & ATA_SR_ERR) continue;

            // Must assert DRQ for ATA 512-byte identify data
            if (ata_wait_drq(port) == 0) {
                unsigned short buf[256];
                for (int i = 0; i < 256; i++) buf[i] = inw(port + 0);

                // Word 0 bit 15 must be 0 for ATA device
                if ((buf[0] & 0x8000) != 0) continue;

                g_drives[drive_count].present = 1;
                g_drives[drive_count].is_atapi = 0;
                g_drives[drive_count].base_port = port;
                g_drives[drive_count].ctrl_port = ctrl;
                g_drives[drive_count].drive_idx = d;
                g_drives[drive_count].lba_offset = 0;

                unsigned int sectors = (unsigned int)buf[60] | ((unsigned int)buf[61] << 16);
                if (buf[83] & (1 << 10)) { // LBA48 supported
                    unsigned int s48 = (unsigned int)buf[100] | ((unsigned int)buf[101] << 16);
                    if (s48 > sectors) sectors = s48;
                }
                g_drives[drive_count].total_sectors = sectors;
                drive_count++;
            }
        }
    }
    return drive_count;
}

int disk_read_drive(int drive, unsigned int lba, unsigned char* buf)
{
    if (drive < 0 || drive >= MAX_DRIVES) return -1;
    disk_drive_t* d = &g_drives[drive];
    if (!d->present) return -1;
    
    unsigned int port = d->base_port;
    if (d->is_atapi) {
        // ATAPI READ 12
        outb(port + 6, (d->drive_idx << 4));
        inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);
        outb(port + 1, 0); // Feature
        outb(port + 4, 2048 & 0xFF); // Byte count limit
        outb(port + 5, 2048 >> 8);
        outb(port + 7, 0xA0); // PACKET
        if (ata_wait_drq(port) != 0) return -1;
        
        unsigned char packet[12] = {0};
        packet[0] = 0xA8; // READ(12)
        packet[2] = (lba >> 24) & 0xFF;
        packet[3] = (lba >> 16) & 0xFF;
        packet[4] = (lba >> 8) & 0xFF;
        packet[5] = (lba & 0xFF);
        packet[9] = 1; // 1 sector (2048 bytes)
        
        unsigned short* ptr = (unsigned short*)packet;
        for (int i=0; i<6; i++) outw(port + 0, ptr[i]);
        
        if (ata_wait_drq(port) != 0) return -1;
        
        unsigned short* buf16 = (unsigned short*)buf;
        for (int i=0; i<1024; i++) {
            buf16[i] = inw(port + 0);
        }
        
        while ((inb(port + 7) & ATA_SR_BSY) != 0);
        return 0;
    }
    
    // Normal ATA
    lba += d->lba_offset;
    if (lba >= 0x0FFFFFFF) {
        // 48-bit LBA Read
        outb(port + 6, 0x40 | (d->drive_idx << 4));
        if (ata_wait_ready(port) != 0) return -1;
        outb(port + 2, 0); outb(port + 2, 1);
        outb(port + 3, (unsigned char)((lba >> 24) & 0xFF));
        outb(port + 3, (unsigned char)(lba & 0xFF));
        outb(port + 4, 0);
        outb(port + 4, (unsigned char)((lba >> 8) & 0xFF));
        outb(port + 5, 0);
        outb(port + 5, (unsigned char)((lba >> 16) & 0xFF));
        outb(port + 7, ATA_CMD_READ_EXT);
    } else {
        // 28-bit LBA Read
        ata_select_drive(port, d->drive_idx, lba);
        if (ata_wait_ready(port) != 0) return -1;
        outb(port + 2, 1);
        outb(port + 3, (unsigned char)(lba & 0xFF));
        outb(port + 4, (unsigned char)((lba >> 8) & 0xFF));
        outb(port + 5, (unsigned char)((lba >> 16) & 0xFF));
        outb(port + 7, ATA_CMD_READ);
    }
    
    if (ata_wait_drq(port) != 0) return -1;
    
    unsigned short* buf16 = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) {
        buf16[i] = inw(port + 0);
    }
    return 0;
}

int disk_write_drive(int drive, unsigned int lba, const unsigned char* buf)
{
    if (drive < 0 || drive >= MAX_DRIVES) return -1;
    disk_drive_t* d = &g_drives[drive];
    if (!d->present || d->is_atapi) return -1;
    
    unsigned int port = d->base_port;
    lba += d->lba_offset;
    if (lba >= 0x0FFFFFFF) {
        // 48-bit LBA Write
        outb(port + 6, 0x40 | (d->drive_idx << 4));
        if (ata_wait_ready(port) != 0) return -1;
        outb(port + 2, 0); outb(port + 2, 1);
        outb(port + 3, (unsigned char)((lba >> 24) & 0xFF));
        outb(port + 3, (unsigned char)(lba & 0xFF));
        outb(port + 4, 0);
        outb(port + 4, (unsigned char)((lba >> 8) & 0xFF));
        outb(port + 5, 0);
        outb(port + 5, (unsigned char)((lba >> 16) & 0xFF));
        outb(port + 7, ATA_CMD_WRITE_EXT);
    } else {
        // 28-bit LBA Write
        ata_select_drive(port, d->drive_idx, lba);
        if (ata_wait_ready(port) != 0) return -1;
        outb(port + 2, 1);
        outb(port + 3, (unsigned char)(lba & 0xFF));
        outb(port + 4, (unsigned char)((lba >> 8) & 0xFF));
        outb(port + 5, (unsigned char)((lba >> 16) & 0xFF));
        outb(port + 7, ATA_CMD_WRITE);
    }
    
    if (ata_wait_drq(port) != 0) return -1;
    
    unsigned short* buf16 = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) {
        outw(port + 0, buf16[i]);
    }
    
    // Per-sector cache flush removed for performance; flushed via disk_flush_all()
    return 0;
}

void disk_flush(int drive)
{
    if (drive < 0 || drive >= MAX_DRIVES) return;
    disk_drive_t* d = &g_drives[drive];
    if (!d->present || d->is_atapi) return;
    unsigned int port = d->base_port;
    outb(port + 6, 0xE0 | (d->drive_idx << 4));
    if (ata_wait_ready(port) != 0) return;
    outb(port + 7, ATA_CMD_CACHE_FLUSH);
    ata_wait_ready(port);
}

void disk_flush_all(void)
{
    for (int i = 0; i < MAX_DRIVES; i++) {
        if (g_drives[i].present && !g_drives[i].is_atapi) {
            disk_flush(i);
        }
    }
}

// Backwards compat: use drive 0
int disk_detect(void) { return disk_detect_all() > 0 ? 0 : -1; }
int disk_read(unsigned int lba, unsigned char* buf) { return disk_read_drive(0, lba, buf); }
int disk_write(unsigned int lba, const unsigned char* buf) { return disk_write_drive(0, lba, buf); }