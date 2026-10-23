#include "iso9660.h"

static int str_case_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        i++;
    }
    return (a[i] == 0 && b[i] == 0);
}

int iso_mount(blkdev_t* dev, iso_fs_t* fs) {
    if (!dev || dev->type != BLK_TYPE_ATAPI) return -1;
    fs->dev = dev;
    fs->mounted = 0;
    fs->is_joliet = 0;
    fs->volume_label[0] = '\0';

    unsigned char sec[2048];
    int found_pvd = 0;

    for (unsigned int lba = 16; lba < 32; lba++) {
        if (dev->read_sectors(dev, lba, 1, sec) != 0) break;
        if (sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' || sec[4] != '0' || sec[5] != '1') {
            break;
        }

        unsigned char type = sec[0];
        if (type == 255) break; // Terminator

        // Check for Joliet Supplementary Volume Descriptor (Type 2)
        if (type == 2) {
            if (sec[88] == 0x25 && sec[89] == 0x2F &&
                (sec[90] == 0x40 || sec[90] == 0x43 || sec[90] == 0x45)) {
                fs->is_joliet = 1;
                unsigned char* root_rec = &sec[156];
                fs->root_extent_lba = *(unsigned int*)&root_rec[2];
                fs->root_data_len = *(unsigned int*)&root_rec[10];
                fs->total_sectors = *(unsigned int*)&sec[80];

                // Joliet Volume Label (UCS-2 BE)
                int lp = 0;
                for (int i = 40; i < 72 && lp < 31; i += 2) {
                    char c = sec[i + 1];
                    if (c == 0 || c == ' ') break;
                    fs->volume_label[lp++] = c;
                }
                fs->volume_label[lp] = '\0';
                fs->mounted = 1;
                return 0; // Joliet is preferred
            }
        } else if (type == 1 && !found_pvd) {
            // Standard Primary Volume Descriptor (Type 1)
            found_pvd = 1;
            unsigned char* root_rec = &sec[156];
            fs->root_extent_lba = *(unsigned int*)&root_rec[2];
            fs->root_data_len = *(unsigned int*)&root_rec[10];
            fs->total_sectors = *(unsigned int*)&sec[80];

            int lp = 0;
            for (int i = 40; i < 72 && lp < 31; i++) {
                char c = sec[i];
                if (c == ' ' && lp > 0 && sec[i + 1] == ' ') break;
                if (c != 0) fs->volume_label[lp++] = c;
            }
            fs->volume_label[lp] = '\0';
            fs->mounted = 1;
        }
    }

    return fs->mounted ? 0 : -1;
}

typedef int (*iso_dirent_visitor_t)(const iso_file_info_t* item, void* user_data);

static int parse_directory(iso_fs_t* fs, unsigned int start_lba, unsigned int total_len, iso_dirent_visitor_t visitor, void* user_data) {
    unsigned char sec[2048];
    unsigned int sectors = (total_len + 2047) / 2048;

    for (unsigned int s = 0; s < sectors; s++) {
        if (fs->dev->read_sectors(fs->dev, start_lba + s, 1, sec) != 0) {
            return -1;
        }

        unsigned int offset = 0;
        while (offset < 2048) {
            unsigned char rec_len = sec[offset];
            if (rec_len == 0) {
                // No more records in this sector; move to next sector
                break;
            }

            unsigned char* rec = &sec[offset];
            unsigned char len_fi = rec[32];
            unsigned char flags = rec[25];

            // Ignore current directory (0) and parent directory (1) special entries
            if (len_fi == 1 && (rec[33] == 0 || rec[33] == 1)) {
                offset += rec_len;
                continue;
            }

            iso_file_info_t item;
            item.extent_lba = *(unsigned int*)&rec[2];
            item.size = *(unsigned int*)&rec[10];
            item.is_dir = (flags & 0x02) ? 1 : 0;

            int name_len = 0;
            if (fs->is_joliet) {
                // Unicode UCS-2 Big Endian -> ASCII
                for (int i = 0; i < len_fi && name_len < 63; i += 2) {
                    char c = rec[33 + i + 1];
                    if (c == 0) c = rec[33 + i];
                    if (c == 0) break;
                    item.name[name_len++] = c;
                }
            } else {
                for (int i = 0; i < len_fi && name_len < 63; i++) {
                    char c = rec[33 + i];
                    if (c == ';') break; // Strip version suffix ;1
                    item.name[name_len++] = c;
                }
                // Strip trailing dot if present
                if (name_len > 0 && item.name[name_len - 1] == '.') {
                    name_len--;
                }
            }
            item.name[name_len] = '\0';

            if (visitor(&item, user_data) != 0) {
                return 1; // Visited item matched / abort early
            }

            offset += rec_len;
        }
    }
    return 0;
}

struct list_ctx {
    void (*cb)(const iso_file_info_t*);
};

static int list_visitor(const iso_file_info_t* item, void* ud) {
    struct list_ctx* ctx = (struct list_ctx*)ud;
    if (ctx->cb) ctx->cb(item);
    return 0;
}

struct find_ctx {
    const char* target_name;
    iso_file_info_t* out_info;
    int found;
};

static int find_visitor(const iso_file_info_t* item, void* ud) {
    struct find_ctx* ctx = (struct find_ctx*)ud;
    if (str_case_eq(item->name, ctx->target_name)) {
        *ctx->out_info = *item;
        ctx->found = 1;
        return 1;
    }
    return 0;
}

static const char* get_next_path_comp(const char* path, char* comp, int max_comp) {
    while (*path == '\\' || *path == '/') path++;
    int i = 0;
    while (*path && *path != '\\' && *path != '/') {
        if (i < max_comp - 1) comp[i++] = *path;
        path++;
    }
    comp[i] = '\0';
    return path;
}

int iso_find_file(iso_fs_t* fs, const char* path, iso_file_info_t* info) {
    if (!fs || !fs->mounted) return -1;

    // Skip drive letter prefix if present (e.g. "E:\" or "C:\")
    if (path[0] && path[1] == ':') {
        path += 2;
        if (*path == '\\' || *path == '/') path++;
    }

    if (*path == '\0') {
        info->extent_lba = fs->root_extent_lba;
        info->size = fs->root_data_len;
        info->is_dir = 1;
        info->name[0] = '\0';
        return 0;
    }

    unsigned int cur_lba = fs->root_extent_lba;
    unsigned int cur_len = fs->root_data_len;
    char comp[64];

    while (*path) {
        path = get_next_path_comp(path, comp, sizeof(comp));
        if (comp[0] == '\0') break;

        struct find_ctx ctx = { comp, info, 0 };
        if (parse_directory(fs, cur_lba, cur_len, find_visitor, &ctx) != 1 || !ctx.found) {
            return -1; // Component not found
        }

        if (*path) {
            if (!info->is_dir) return -1;
            cur_lba = info->extent_lba;
            cur_len = info->size;
        }
    }

    return 0;
}

int iso_list_dir(iso_fs_t* fs, const char* path, void (*callback)(const iso_file_info_t*)) {
    if (!fs || !fs->mounted) return -1;
    iso_file_info_t dir_info;
    if (iso_find_file(fs, path, &dir_info) != 0) return -1;
    if (!dir_info.is_dir) return -1;

    struct list_ctx ctx = { callback };
    parse_directory(fs, dir_info.extent_lba, dir_info.size, list_visitor, &ctx);
    return 0;
}

int iso_read_file(iso_fs_t* fs, const iso_file_info_t* info, unsigned int offset, unsigned char* buf, unsigned int size) {
    if (!fs || !fs->mounted || !info || info->is_dir) return -1;
    if (offset >= info->size) return 0;
    if (offset + size > info->size) {
        size = info->size - offset;
    }

    unsigned int bytes_read = 0;
    unsigned char sec[2048];

    while (bytes_read < size) {
        unsigned int cur_offset = offset + bytes_read;
        unsigned int sec_idx = cur_offset / 2048;
        unsigned int sec_off = cur_offset % 2048;

        if (fs->dev->read_sectors(fs->dev, info->extent_lba + sec_idx, 1, sec) != 0) {
            break;
        }

        unsigned int to_copy = 2048 - sec_off;
        if (to_copy > size - bytes_read) to_copy = size - bytes_read;

        for (unsigned int i = 0; i < to_copy; i++) {
            buf[bytes_read + i] = sec[sec_off + i];
        }
        bytes_read += to_copy;
    }

    return bytes_read;
}
