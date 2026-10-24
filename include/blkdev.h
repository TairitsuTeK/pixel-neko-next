#ifndef BLKDEV_H
#define BLKDEV_H

typedef enum {
    BLK_TYPE_NONE = 0,
    BLK_TYPE_FLOPPY,   // Floppy disk (A: or B:)
    BLK_TYPE_ATA,      // Hard disk (ATA PIO)
    BLK_TYPE_ATAPI     // Optical drive (CD/DVD)
} blk_type_t;

typedef enum {
    MEDIA_UNKNOWN = 0,
    MEDIA_PRESENT,
    MEDIA_ABSENT,
    MEDIA_ERROR
} media_state_t;

typedef struct blkdev {
    int           id;              // 0..MAX_BLKDEV-1
    blk_type_t    type;
    int           unit;            // Unit index within subsystem
    int           is_readonly;     // 1 = read-only (CD-ROM or write-protected)
    unsigned int  sector_size;     // 512 for HDD/floppy, 2048 for CD-ROM
    unsigned int  total_sectors;
    media_state_t media_state;
    unsigned int  media_gen;       // Media change generation
    char          model[48];

    int (*read_sectors)(struct blkdev* dev, unsigned int lba, unsigned int count, unsigned char* buf);
    int (*write_sectors)(struct blkdev* dev, unsigned int lba, unsigned int count, const unsigned char* buf);
    media_state_t (*check_media)(struct blkdev* dev);
} blkdev_t;

#define MAX_BLKDEV 16
extern blkdev_t g_blkdevs[MAX_BLKDEV];

void blkdev_init(void);
blkdev_t* blkdev_get(int id);
int blkdev_count(void);

#endif
