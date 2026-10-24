#include "vfs.h"
#include "iso9660.h"
#include "fat.h"
#include "fdc.h"

vfs_drive_t g_vfs_drives[26];
static iso_fs_t g_iso_fs[4];

static void my_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

const char* vfs_get_fs_name(part_fs_type_t type) {
    switch (type) {
        case PART_FS_FAT12: return "FAT12";
        case PART_FS_FAT16: return "FAT16";
        case PART_FS_FAT32: return "FAT32";
        case PART_FS_NTFS:  return "NTFS";
        case PART_FS_EXFAT: return "exFAT";
        case PART_FS_EXT:   return "EXT";
        default:            return "Unknown";
    }
}

vfs_drive_t* vfs_get_drive(char letter) {
    if (letter >= 'a' && letter <= 'z') letter -= 32;
    if (letter < 'A' || letter > 'Z') return 0;
    int idx = letter - 'A';
    if (!g_vfs_drives[idx].present) return 0;
    return &g_vfs_drives[idx];
}

void vfs_init(void) {
    for (int i = 0; i < 26; i++) {
        g_vfs_drives[i].letter = 'A' + i;
        g_vfs_drives[i].present = 0;
        g_vfs_drives[i].mounted = 0;
        g_vfs_drives[i].label[0] = '\0';
    }

    // Initialize underlying blocks and partitions
    blkdev_init();
    part_scan_all();

    // 1. Floppy drives: fixed assignment to A: and B:
    if (fdc_has_drive(0)) {
        vfs_drive_t* d = &g_vfs_drives[0]; // A:
        d->present = 1;
        d->drv_type = VFS_DRV_FLOPPY;
        d->fs_type = PART_FS_FAT12;
        d->is_supported = 1;
        d->is_readonly = 0;
        d->dev = blkdev_get(0);
        d->start_lba = 0;
        d->total_sectors = fdc_get_total_sectors(0);
        d->total_kb = 1440;
        d->free_kb = 0;
        d->is_free_known = 0;
        my_strcpy(d->label, "3.5\" Floppy (A:)", sizeof(d->label));
    }

    if (fdc_has_drive(1)) {
        vfs_drive_t* d = &g_vfs_drives[1]; // B:
        d->present = 1;
        d->drv_type = VFS_DRV_FLOPPY;
        d->fs_type = PART_FS_FAT12;
        d->is_supported = 1;
        d->is_readonly = 0;
        d->dev = blkdev_get(1);
        d->start_lba = 0;
        d->total_sectors = fdc_get_total_sectors(1);
        d->total_kb = 1440;
        d->free_kb = 0;
        d->is_free_known = 0;
        my_strcpy(d->label, "3.5\" Floppy (B:)", sizeof(d->label));
    }

    // 2. Hard disk partitions: assigned starting from C:
    char next_letter = 'C';

    for (int p = 0; p < g_partition_count && next_letter <= 'Z'; p++) {
        partition_entry_t* part = &g_partitions[p];
        int idx = next_letter - 'A';
        vfs_drive_t* d = &g_vfs_drives[idx];

        d->present = 1;
        d->drv_type = VFS_DRV_HDD;
        d->fs_type = part->fs_type;
        d->is_supported = part->is_supported;
        d->is_readonly = 0;
        d->dev = part->dev;
        d->start_lba = part->start_lba;
        d->total_sectors = part->total_sectors;
        d->total_kb = (unsigned int)(((unsigned long long)d->total_sectors * 512) / 1024);
        d->free_kb = 0;
        d->is_free_known = 0;
        my_strcpy(d->label, part->label, sizeof(d->label));

        next_letter++;
    }

    // 3. Optical drives (CD/DVD): immediately follow hard disk partitions.
    int b_count = blkdev_count();
    int cd_idx = 0;

    for (int i = 0; i < b_count && next_letter <= 'Z'; i++) {
        blkdev_t* dev = blkdev_get(i);
        if (dev && dev->type == BLK_TYPE_ATAPI) {
            int idx = next_letter - 'A';
            vfs_drive_t* d = &g_vfs_drives[idx];

            d->present = 1;
            d->drv_type = VFS_DRV_CDROM;
            d->fs_type = PART_FS_UNKNOWN; // CDFS / ISO9660
            d->is_supported = 1;
            d->is_readonly = 1;
            d->dev = dev;
            d->start_lba = 0;
            d->total_sectors = 0;
            d->total_kb = 0;
            d->free_kb = 0;
            d->is_free_known = 1;

            // Attempt ISO9660 mount
            if (cd_idx < 4) {
                if (iso_mount(dev, &g_iso_fs[cd_idx]) == 0) {
                    d->mounted = 1;
                    d->total_sectors = g_iso_fs[cd_idx].total_sectors;
                    d->total_kb = d->total_sectors * 2;
                    if (g_iso_fs[cd_idx].volume_label[0]) {
                        my_strcpy(d->label, g_iso_fs[cd_idx].volume_label, sizeof(d->label));
                    } else {
                        my_strcpy(d->label, "CD-ROM Disc", sizeof(d->label));
                    }
                } else {
                    my_strcpy(d->label, "CD-ROM Drive", sizeof(d->label));
                }
                cd_idx++;
            }
            next_letter++;
        }
    }

    // Initialize FAT structures
    fat_init();
}

struct vfs_list_bridge {
    void (*user_cb)(const vfs_dirent_t*);
};

static void vfs_iso_cb(const iso_file_info_t* info) {
    extern struct vfs_list_bridge g_cur_list_bridge;
    vfs_dirent_t ent;
    my_strcpy(ent.name, info->name, sizeof(ent.name));
    ent.is_dir = info->is_dir;
    ent.size = info->size;
    ent.first_cluster = info->extent_lba;
    if (g_cur_list_bridge.user_cb) {
        g_cur_list_bridge.user_cb(&ent);
    }
}

static void vfs_fat_cb(const file_info_t* info) {
    extern struct vfs_list_bridge g_cur_list_bridge;
    vfs_dirent_t ent;
    my_strcpy(ent.name, info->name, sizeof(ent.name));
    ent.is_dir = (info->attr & 0x10) ? 1 : 0;
    ent.size = info->size;
    ent.first_cluster = info->first_cluster;
    if (g_cur_list_bridge.user_cb) {
        g_cur_list_bridge.user_cb(&ent);
    }
}

struct vfs_list_bridge g_cur_list_bridge;

int vfs_list_dir(const char* path, void (*callback)(const vfs_dirent_t*)) {
    if (!path || !path[0]) return VFS_ERR_NOT_FOUND;
    char letter = path[0];
    vfs_drive_t* drv = vfs_get_drive(letter);
    if (!drv) return VFS_ERR_NO_DRIVE;

    // Check if the filesystem on this partition is supported
    if (!drv->is_supported) {
        return VFS_ERR_UNSUPPORTED; // E.g. NTFS, exFAT, ext
    }

    // Check media presence for removable drives
    if (drv->drv_type == VFS_DRV_FLOPPY) {
        if (fdc_check_media(drv->dev->unit) == MEDIA_ABSENT) {
            return VFS_ERR_NO_MEDIA;
        }
    } else if (drv->drv_type == VFS_DRV_CDROM) {
        if (drv->dev->check_media && drv->dev->check_media(drv->dev) == MEDIA_ABSENT) {
            return VFS_ERR_NO_MEDIA;
        }
        // If not mounted, try mounting now
        int cd_idx = 0;
        if (!drv->mounted) {
            if (iso_mount(drv->dev, &g_iso_fs[cd_idx]) == 0) {
                drv->mounted = 1;
                if (g_iso_fs[cd_idx].volume_label[0]) {
                    my_strcpy(drv->label, g_iso_fs[cd_idx].volume_label, sizeof(drv->label));
                }
            } else {
                return VFS_ERR_NO_MEDIA;
            }
        }
    }

    g_cur_list_bridge.user_cb = callback;

    if (drv->drv_type == VFS_DRV_CDROM) {
        const char* subpath = path + 2;
        if (*subpath == '\\' || *subpath == '/') subpath++;
        return iso_list_dir(&g_iso_fs[0], subpath, vfs_iso_cb);
    } else {
        // FAT volume listing
        return fat_list_dir(path, vfs_fat_cb);
    }
}

int vfs_find_file(const char* path, vfs_dirent_t* out_entry) {
    if (!path || !path[0]) return VFS_ERR_NOT_FOUND;
    char letter = path[0];
    vfs_drive_t* drv = vfs_get_drive(letter);
    if (!drv) return VFS_ERR_NO_DRIVE;
    if (!drv->is_supported) return VFS_ERR_UNSUPPORTED;

    if (drv->drv_type == VFS_DRV_CDROM) {
        const char* subpath = path + 2;
        if (*subpath == '\\' || *subpath == '/') subpath++;
        iso_file_info_t iso_info;
        if (iso_find_file(&g_iso_fs[0], subpath, &iso_info) != 0) {
            return VFS_ERR_NOT_FOUND;
        }
        my_strcpy(out_entry->name, iso_info.name, sizeof(out_entry->name));
        out_entry->is_dir = iso_info.is_dir;
        out_entry->size = iso_info.size;
        out_entry->first_cluster = iso_info.extent_lba;
        return VFS_OK;
    } else {
        file_info_t finfo;
        if (fat_find(path, &finfo) != 0) {
            return VFS_ERR_NOT_FOUND;
        }
        my_strcpy(out_entry->name, finfo.name, sizeof(out_entry->name));
        out_entry->is_dir = (finfo.attr & 0x10) ? 1 : 0;
        out_entry->size = finfo.size;
        out_entry->first_cluster = finfo.first_cluster;
        return VFS_OK;
    }
}

int vfs_read_file(const char* path, unsigned char* buf, unsigned int max_size) {
    if (!path || !path[0]) return -1;
    char letter = path[0];
    vfs_drive_t* drv = vfs_get_drive(letter);
    if (!drv || !drv->is_supported) return -1;

    if (drv->drv_type == VFS_DRV_CDROM) {
        const char* subpath = path + 2;
        if (*subpath == '\\' || *subpath == '/') subpath++;
        iso_file_info_t iso_info;
        if (iso_find_file(&g_iso_fs[0], subpath, &iso_info) != 0) return -1;
        return iso_read_file(&g_iso_fs[0], &iso_info, 0, buf, max_size);
    } else {
        file_handle_t* h = fat_open(path);
        if (!h) return -1;
        int bytes = fat_read(h, buf, max_size);
        fat_close(h);
        return bytes;
    }
}

int vfs_write_file(const char* path, const unsigned char* buf, unsigned int size) {
    if (!path || !path[0]) return VFS_ERR_NOT_FOUND;
    char letter = path[0];
    vfs_drive_t* drv = vfs_get_drive(letter);
    if (!drv) return VFS_ERR_NO_DRIVE;
    if (drv->is_readonly) return VFS_ERR_READONLY;
    if (!drv->is_supported) return VFS_ERR_UNSUPPORTED;

    if (fat_write_file(path, buf, size) != 0) {
        return VFS_ERR_IO;
    }
    return VFS_OK;
}

int vfs_get_drive_space(char letter, unsigned int* total_kb, unsigned int* free_kb, int* is_known) {
    vfs_drive_t* drv = vfs_get_drive(letter);
    if (!drv) return -1;

    if (drv->drv_type == VFS_DRV_CDROM) {
        if (total_kb) *total_kb = drv->total_kb;
        if (free_kb) *free_kb = 0;
        if (is_known) *is_known = 1;
        return 0;
    }

    if (drv->drv_type == VFS_DRV_FLOPPY) {
        unsigned int tot = 1440, fre = 0;
        if (fat_get_volume_space(letter, &tot, &fre) == 0) {
            drv->total_kb = tot;
            drv->free_kb = fre;
            drv->is_free_known = 1;
            if (total_kb) *total_kb = tot;
            if (free_kb) *free_kb = fre;
            if (is_known) *is_known = 1;
            return 0;
        } else {
            if (total_kb) *total_kb = 1440;
            if (free_kb) *free_kb = 0;
            if (is_known) *is_known = 0;
            return 0;
        }
    }

    // Hard disk partition
    if (drv->is_supported) {
        unsigned int tot = drv->total_kb, fre = 0;
        if (fat_get_volume_space(letter, &tot, &fre) == 0) {
            drv->total_kb = tot;
            drv->free_kb = fre;
            drv->is_free_known = 1;
            if (total_kb) *total_kb = tot;
            if (free_kb) *free_kb = fre;
            if (is_known) *is_known = 1;
            return 0;
        }
    }

    // Fallback or unsupported FS (NTFS/EXT)
    if (total_kb) *total_kb = drv->total_kb;
    if (free_kb) *free_kb = 0;
    if (is_known) *is_known = 0;
    return 0;
}

int vfs_stream_open(const char* path, vfs_stream_t* s) {
    if (!path || !path[0] || !s) return -1;
    char letter = path[0];
    vfs_drive_t* drv = vfs_get_drive(letter);
    if (!drv || !drv->is_supported) return -1;

    s->current_offset = 0;
    s->is_open = 0;
    int i = 0; while (path[i] && i < 255) { s->path[i] = path[i]; i++; }
    s->path[i] = '\0';

    if (drv->drv_type == VFS_DRV_CDROM) {
        const char* subpath = path + 2;
        if (*subpath == '\\' || *subpath == '/') subpath++;
        iso_file_info_t iso_info;
        if (iso_find_file(&g_iso_fs[0], subpath, &iso_info) != 0) return -1;
        s->size = iso_info.size;
        s->is_cdrom = 1;
        s->cdrom_extent_lba = iso_info.extent_lba;
        s->fat_handle = 0;
        s->is_open = 1;
        return 0;
    } else {
        file_handle_t* h = fat_open(path);
        if (!h) return -1;
        s->size = h->size;
        s->is_cdrom = 0;
        s->cdrom_extent_lba = 0;
        s->fat_handle = (void*)h;
        s->is_open = 1;
        return 0;
    }
}

int vfs_stream_read(vfs_stream_t* s, void* buf, unsigned int size) {
    if (!s || !s->is_open) return -1;
    if (s->current_offset >= s->size) return 0;
    unsigned int remaining = s->size - s->current_offset;
    if (size > remaining) size = remaining;
    if (size == 0) return 0;

    if (s->is_cdrom) {
        iso_file_info_t iso_info;
        iso_info.extent_lba = s->cdrom_extent_lba;
        iso_info.size = s->size;
        int n = iso_read_file(&g_iso_fs[0], &iso_info, s->current_offset, (unsigned char*)buf, size);
        if (n > 0) s->current_offset += n;
        return n;
    } else {
        int n = fat_read((file_handle_t*)s->fat_handle, (unsigned char*)buf, size);
        if (n > 0) s->current_offset += n;
        return n;
    }
}

int vfs_stream_seek(vfs_stream_t* s, unsigned int offset) {
    if (!s || !s->is_open) return -1;
    if (offset > s->size) offset = s->size;
    s->current_offset = offset;
    if (!s->is_cdrom && s->fat_handle) {
        fat_seek((file_handle_t*)s->fat_handle, offset);
    }
    return 0;
}

void vfs_stream_close(vfs_stream_t* s) {
    if (s && s->is_open) {
        if (!s->is_cdrom && s->fat_handle) {
            fat_close((file_handle_t*)s->fat_handle);
            s->fat_handle = 0;
        }
        s->is_open = 0;
    }
}

