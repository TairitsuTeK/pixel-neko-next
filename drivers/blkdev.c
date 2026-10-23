#include "blkdev.h"
#include "disk.h"
#include "fdc.h"

blkdev_t g_blkdevs[MAX_BLKDEV];
static int g_blkdev_count = 0;

static void my_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int floppy_read(blkdev_t* dev, unsigned int lba, unsigned int count, unsigned char* buf) {
    return fdc_read_sectors(dev->unit, lba, count, buf);
}

static int floppy_write(blkdev_t* dev, unsigned int lba, unsigned int count, const unsigned char* buf) {
    return fdc_write_sectors(dev->unit, lba, count, buf);
}

static media_state_t floppy_check(blkdev_t* dev) {
    return fdc_check_media(dev->unit);
}

static int ata_read(blkdev_t* dev, unsigned int lba, unsigned int count, unsigned char* buf) {
    for (unsigned int i = 0; i < count; i++) {
        if (disk_read_drive(dev->unit, lba + i, buf + i * dev->sector_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ata_write(blkdev_t* dev, unsigned int lba, unsigned int count, const unsigned char* buf) {
    if (dev->is_readonly) return -1;
    for (unsigned int i = 0; i < count; i++) {
        if (disk_write_drive(dev->unit, lba + i, buf + i * dev->sector_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static media_state_t ata_check(blkdev_t* dev) {
    (void)dev;
    return MEDIA_PRESENT;
}

static media_state_t atapi_check(blkdev_t* dev) {
    // ATAPI optical media check: try reading sector 16 (PVD)
    unsigned char test_buf[2048];
    if (disk_read_drive(dev->unit, 16, test_buf) == 0) {
        return MEDIA_PRESENT;
    }
    return MEDIA_ABSENT;
}

void blkdev_init(void) {
    g_blkdev_count = 0;
    for (int i = 0; i < MAX_BLKDEV; i++) {
        g_blkdevs[i].id = i;
        g_blkdevs[i].type = BLK_TYPE_NONE;
        g_blkdevs[i].media_state = MEDIA_UNKNOWN;
    }

    // 1. Floppy controller detection (CMOS 0x10)
    fdc_init();
    for (int f = 0; f < 2; f++) {
        if (fdc_has_drive(f) && g_blkdev_count < MAX_BLKDEV) {
            blkdev_t* dev = &g_blkdevs[g_blkdev_count];
            dev->id = g_blkdev_count;
            dev->type = BLK_TYPE_FLOPPY;
            dev->unit = f;
            dev->is_readonly = 0;
            dev->sector_size = 512;
            dev->total_sectors = fdc_get_total_sectors(f);
            dev->media_state = MEDIA_UNKNOWN;
            dev->media_gen = 0;
            if (f == 0) my_strcpy(dev->model, "3.5\" 1.44M Floppy (A:)", sizeof(dev->model));
            else my_strcpy(dev->model, "3.5\" 1.44M Floppy (B:)", sizeof(dev->model));
            dev->read_sectors = floppy_read;
            dev->write_sectors = floppy_write;
            dev->check_media = floppy_check;
            g_blkdev_count++;
        }
    }

    // 2. ATA & ATAPI IDE detection
    disk_detect_all();
    for (int d = 0; d < MAX_DRIVES; d++) {
        if (g_drives[d].present && g_blkdev_count < MAX_BLKDEV) {
            blkdev_t* dev = &g_blkdevs[g_blkdev_count];
            dev->id = g_blkdev_count;
            dev->unit = d;
            dev->media_gen = 0;

            if (g_drives[d].is_atapi) {
                dev->type = BLK_TYPE_ATAPI;
                dev->is_readonly = 1;
                dev->sector_size = 2048;
                dev->total_sectors = 0; // Determined on media load
                dev->media_state = MEDIA_UNKNOWN;
                my_strcpy(dev->model, "ATAPI CD-ROM / DVD Drive", sizeof(dev->model));
                dev->read_sectors = ata_read;
                dev->write_sectors = ata_write; // Will reject writes
                dev->check_media = atapi_check;
            } else {
                dev->type = BLK_TYPE_ATA;
                dev->is_readonly = 0;
                dev->sector_size = 512;
                dev->total_sectors = g_drives[d].total_sectors;
                dev->media_state = MEDIA_PRESENT;
                my_strcpy(dev->model, "IDE Fixed Hard Disk", sizeof(dev->model));
                dev->read_sectors = ata_read;
                dev->write_sectors = ata_write;
                dev->check_media = ata_check;
            }
            g_blkdev_count++;
        }
    }
}

blkdev_t* blkdev_get(int id) {
    if (id < 0 || id >= g_blkdev_count) return 0;
    return &g_blkdevs[id];
}

int blkdev_count(void) {
    return g_blkdev_count;
}
