#ifndef PART_H
#define PART_H

#include "blkdev.h"

typedef enum {
    PART_FS_UNKNOWN = 0,
    PART_FS_FAT12,
    PART_FS_FAT16,
    PART_FS_FAT32,
    PART_FS_NTFS,
    PART_FS_EXFAT,
    PART_FS_EXT
} part_fs_type_t;

typedef struct {
    blkdev_t*      dev;
    unsigned int   start_lba;
    unsigned int   total_sectors;
    unsigned char  sys_id;          // Partition type byte (e.g. 0x07, 0x0B, 0x83)
    part_fs_type_t fs_type;
    int            is_supported;    // 1 if kernel can mount & read (FAT12/16/32), 0 otherwise (NTFS/exFAT/ext)
    char           label[32];
} partition_entry_t;

#define MAX_PARTITIONS 16
extern partition_entry_t g_partitions[MAX_PARTITIONS];
extern int g_partition_count;

void part_scan_all(void);

#endif
