#include "part.h"

partition_entry_t g_partitions[MAX_PARTITIONS];
int g_partition_count = 0;

static void my_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int is_hidden_or_special(unsigned char type) {
    if (type == 0x00) return 1; // Empty
    if (type == 0x05 || type == 0x0F || type == 0x85) return 1; // Extended container
    if (type == 0xEE) return 1; // GPT Protective MBR
    if (type == 0xEF) return 1; // EFI System Partition (ESP)
    if (type == 0x82) return 1; // Linux Swap
    // Hidden / OEM / Recovery partitions
    if (type == 0x11 || type == 0x14 || type == 0x16 || type == 0x1B ||
        type == 0x1C || type == 0x12 || type == 0x27) return 1;
    return 0;
}

static void probe_filesystem(blkdev_t* dev, unsigned int lba, partition_entry_t* part) {
    unsigned char sec[512];
    if (dev->read_sectors(dev, lba, 1, sec) != 0) {
        part->fs_type = PART_FS_UNKNOWN;
        part->is_supported = 0;
        return;
    }

    // Check for NTFS
    if (sec[3] == 'N' && sec[4] == 'T' && sec[5] == 'F' && sec[6] == 'S') {
        part->fs_type = PART_FS_NTFS;
        part->is_supported = 0;
        my_strcpy(part->label, "NTFS Volume", sizeof(part->label));
        return;
    }

    // Check for exFAT
    if (sec[3] == 'E' && sec[4] == 'X' && sec[5] == 'F' && sec[6] == 'A' && sec[7] == 'T') {
        part->fs_type = PART_FS_EXFAT;
        part->is_supported = 0;
        my_strcpy(part->label, "exFAT Volume", sizeof(part->label));
        return;
    }

    // Check for FAT BPB
    unsigned short bytes_per_sector = *(unsigned short*)&sec[11];
    unsigned char spc = sec[13];
    unsigned short res_sec = *(unsigned short*)&sec[14];
    unsigned char num_fats = sec[16];
    unsigned short root_ent = *(unsigned short*)&sec[17];
    unsigned short tot_sec16 = *(unsigned short*)&sec[19];
    unsigned int tot_sec32 = *(unsigned int*)&sec[32];
    unsigned int fat_sz16 = *(unsigned short*)&sec[22];
    unsigned int fat_sz32 = *(unsigned int*)&sec[36];

    if (bytes_per_sector == 512 && spc > 0 && num_fats > 0 && num_fats <= 4 && res_sec > 0) {
        unsigned int fat_sz = fat_sz16 ? fat_sz16 : fat_sz32;
        unsigned int tot_sec = tot_sec16 ? tot_sec16 : tot_sec32;
        unsigned int root_sec = ((root_ent * 32) + 511) / 512;
        if (tot_sec > (res_sec + num_fats * fat_sz + root_sec)) {
            unsigned int data_sec = tot_sec - (res_sec + num_fats * fat_sz + root_sec);
            unsigned int clusters = data_sec / spc;

            if (clusters < 4085) {
                part->fs_type = PART_FS_FAT12;
                part->is_supported = 1;
                my_strcpy(part->label, "FAT12 Volume", sizeof(part->label));
            } else if (clusters < 65525) {
                part->fs_type = PART_FS_FAT16;
                part->is_supported = 1;
                my_strcpy(part->label, "FAT16 Volume", sizeof(part->label));
            } else {
                part->fs_type = PART_FS_FAT32;
                part->is_supported = 1;
                my_strcpy(part->label, "FAT32 Volume", sizeof(part->label));
            }
            return;
        }
    }

    // Check for Linux ext
    if (part->sys_id == 0x83) {
        part->fs_type = PART_FS_EXT;
        part->is_supported = 0;
        my_strcpy(part->label, "Linux ext Volume", sizeof(part->label));
        return;
    }

    part->fs_type = PART_FS_UNKNOWN;
    part->is_supported = 0;
    my_strcpy(part->label, "Unformatted / Unknown", sizeof(part->label));
}

static void scan_drive_partitions(blkdev_t* dev) {
    unsigned char mbr[512];
    if (dev->read_sectors(dev, 0, 1, mbr) != 0) return;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        // No MBR signature; check if whole disk is superfloppy FAT
        partition_entry_t part;
        part.dev = dev;
        part.start_lba = 0;
        part.total_sectors = dev->total_sectors;
        part.sys_id = 0x06;
        probe_filesystem(dev, 0, &part);
        if (part.is_supported && g_partition_count < MAX_PARTITIONS) {
            g_partitions[g_partition_count++] = part;
        }
        return;
    }

    // Check primary partitions 1..4 (offsets 0x1BE, 0x1CE, 0x1DE, 0x1EE)
    unsigned int ext_lba = 0;
    for (int p = 0; p < 4; p++) {
        unsigned char* ent = &mbr[0x1BE + p * 16];
        unsigned char type = ent[4];
        unsigned int start_lba = *(unsigned int*)&ent[8];
        unsigned int sectors = *(unsigned int*)&ent[12];

        if (sectors == 0) continue;

        if (type == 0x05 || type == 0x0F || type == 0x85) {
            ext_lba = start_lba;
            continue;
        }

        if (is_hidden_or_special(type)) continue;

        if (g_partition_count < MAX_PARTITIONS) {
            partition_entry_t* part = &g_partitions[g_partition_count++];
            part->dev = dev;
            part->start_lba = start_lba;
            part->total_sectors = sectors;
            part->sys_id = type;
            probe_filesystem(dev, start_lba, part);
        }
    }

    // If extended partition exists, parse EBR linked list
    if (ext_lba != 0) {
        unsigned int current_ebr = ext_lba;
        int depth = 0;
        while (current_ebr != 0 && depth < 32) {
            unsigned char ebr[512];
            if (dev->read_sectors(dev, current_ebr, 1, ebr) != 0) break;
            if (ebr[510] != 0x55 || ebr[511] != 0xAA) break;

            // First entry: logical partition
            unsigned char* ent1 = &ebr[0x1BE];
            unsigned char type1 = ent1[4];
            unsigned int start1 = *(unsigned int*)&ent1[8];
            unsigned int sec1 = *(unsigned int*)&ent1[12];

            if (sec1 > 0 && !is_hidden_or_special(type1) && g_partition_count < MAX_PARTITIONS) {
                partition_entry_t* part = &g_partitions[g_partition_count++];
                part->dev = dev;
                part->start_lba = current_ebr + start1;
                part->total_sectors = sec1;
                part->sys_id = type1;
                probe_filesystem(dev, part->start_lba, part);
            }

            // Second entry: next EBR relative to extended partition start
            unsigned char* ent2 = &ebr[0x1CE];
            unsigned char type2 = ent2[4];
            unsigned int start2 = *(unsigned int*)&ent2[8];
            if (type2 != 0 && start2 != 0) {
                current_ebr = ext_lba + start2;
            } else {
                break;
            }
            depth++;
        }
    }
}

void part_scan_all(void) {
    g_partition_count = 0;
    int count = blkdev_count();
    for (int i = 0; i < count; i++) {
        blkdev_t* dev = blkdev_get(i);
        if (dev && dev->type == BLK_TYPE_ATA) {
            scan_drive_partitions(dev);
        }
    }
}
