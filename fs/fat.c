#include "calc.h"
#include "font.h"
#include "wm.h"
#include "fat.h"
#include "disk.h"

#define SECTOR_SIZE 512

fat_fs_t g_volumes[MAX_VOLUMES];
static file_handle_t g_handle;

static int str_casecmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void fat_name_to_str(const unsigned char* raw_name, char* out) {
    int p = 0;
    for (int i = 0; i < 8; i++) {
        if (raw_name[i] == ' ' || raw_name[i] == 0) break;
        out[p++] = raw_name[i];
    }
    if (raw_name[8] != ' ' && raw_name[8] != 0) {
        out[p++] = '.';
        for (int i = 8; i < 11; i++) {
            if (raw_name[i] == ' ' || raw_name[i] == 0) break;
            out[p++] = raw_name[i];
        }
    }
    out[p] = '\0';
}

static void str_to_fat_name(const char* input, unsigned char* name, unsigned char* ext) {
    for (int i = 0; i < 8; i++) name[i] = ' ';
    for (int i = 0; i < 3; i++) ext[i] = ' ';
    int dot_pos = -1, len = str_len(input);
    for (int i = 0; i < len; i++) {
        if (input[i] == '.') { dot_pos = i; break; }
    }
    int main_len = (dot_pos >= 0) ? dot_pos : len;
    if (main_len > 8) main_len = 8;
    for (int i = 0; i < main_len; i++) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        name[i] = c;
    }
    if (dot_pos >= 0) {
        int ext_start = dot_pos + 1, ext_len = len - ext_start;
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
            char c = input[ext_start + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            ext[i] = c;
        }
    }
}

// Drive parser: e.g. "C:\dir\file.txt" -> volume 0, returns "dir\file.txt"
static fat_fs_t* get_vol(const char** path) {
    int v = 0; // Default C:
    if (*path && (*path)[0] >= 'A' && (*path)[0] <= 'Z' && (*path)[1] == ':') {
        v = (*path)[0] - 'C';
        if (v < 0 || v >= MAX_VOLUMES) v = 0;
        *path += 2;
        if ((*path)[0] == '\\' || (*path)[0] == '/') (*path)++;
    } else if (*path && ((*path)[0] == '\\' || (*path)[0] == '/')) {
        (*path)++;
    }
    if (v >= 0 && v < MAX_VOLUMES && g_volumes[v].type != 0) return &g_volumes[v];
    return 0;
}

// Split next path component: "dir\file.txt" -> comp="dir", returns "file.txt"
// Returns NULL if no more components
static const char* split_path(const char* path, char* comp) {
    int i = 0;
    while (path[i] && path[i] != '\\' && path[i] != '/') {
        if (i < 12) comp[i] = path[i];
        i++;
    }
    comp[i < 12 ? i : 12] = '\0';
    if (path[i] == '\\' || path[i] == '/') return path + i + 1;
    return path + i;
}

int mount_fat(fat_fs_t* vol, int drive_idx, unsigned int lba, unsigned char* boot_sector) {
    bpb_t* bpb = (bpb_t*)&boot_sector[11];
    
    if (bpb->bytes_per_sector != 512) return -1;
    if (bpb->sectors_per_cluster == 0) return -1;
    if (bpb->num_fats == 0 || bpb->num_fats > 4) return -1;
    
    unsigned int root_sectors = ((bpb->root_entries * 32) + 511) / 512;
    unsigned int fat_size = bpb->sectors_per_fat ? bpb->sectors_per_fat : bpb->sectors_per_fat_32;
    unsigned int total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    unsigned int data_sectors = total_sectors - (bpb->reserved_sectors + (bpb->num_fats * fat_size) + root_sectors);
    unsigned int total_clusters = data_sectors / bpb->sectors_per_cluster;
    
    if (total_clusters < 4085) vol->type = 12;
    else if (total_clusters < 65525) vol->type = 16;
    else vol->type = 32;

    vol->drive_idx = drive_idx;
    vol->partition_lba = lba;
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->reserved_sectors   = bpb->reserved_sectors;
    vol->num_fats           = bpb->num_fats;
    vol->root_entries       = bpb->root_entries;
    vol->sectors_per_fat    = fat_size;
    vol->root_dir_sectors   = root_sectors;
    vol->total_clusters     = total_clusters;
    vol->first_data_sector  = bpb->reserved_sectors + (bpb->num_fats * fat_size) + root_sectors;
    
    if (vol->type == 32) vol->root_cluster = bpb->root_cluster;
    else vol->root_cluster = 0; 
    
    vol->cached_fat_sector = 0xFFFFFFFF;
    vol->free_clusters = 0;
    vol->is_free_calculated = 0;
    return 0;
}

int fat_mount_volume(int vol_idx, int drive_idx, unsigned int lba) {
    if (vol_idx < 0 || vol_idx >= MAX_VOLUMES) return -1;
    unsigned char sec[512];
    if (disk_read_drive(drive_idx, lba, sec) != 0) return -1;
    return mount_fat(&g_volumes[vol_idx], drive_idx, lba, sec);
}

int fat_init(void) {
    for (int i=0; i<MAX_VOLUMES; i++) g_volumes[i].type = 0;
    disk_detect_all();
    
    int vol_idx = 0;
    for (int d=0; d<4; d++) {
        if (!g_drives[d].present) continue;
        unsigned char sec[2048] = {0};
        if (disk_read_drive(d, 0, sec) != 0) continue;
        
        if (sec[510] == 0x55 && sec[511] == 0xAA) {
            if (sec[0] == 0xEB || sec[0] == 0xE9) {
                if (mount_fat(&g_volumes[vol_idx], d, 0, sec) == 0) vol_idx++;
            } else {
                unsigned int lba = *(unsigned int*)&sec[454];
                if (lba > 0 && disk_read_drive(d, lba, sec) == 0) {
                    if (mount_fat(&g_volumes[vol_idx], d, lba, sec) == 0) vol_idx++;
                }
            }
        }
    }
    return 0;
}

static unsigned int fat_read_entry(fat_fs_t* vol, unsigned int cluster) {
    if (vol->type == 12) {
        unsigned int fat_offset = cluster + (cluster / 2);
        unsigned int fat_sector = vol->reserved_sectors + (fat_offset / 512);
        unsigned int ent_offset = fat_offset % 512;
        unsigned char b0, b1;

        if (vol->cached_fat_sector != fat_sector) {
            if (disk_read_drive(vol->drive_idx, vol->partition_lba + fat_sector, vol->fat_sector_buf) != 0) return 0x0FFFFFFF;
            vol->cached_fat_sector = fat_sector;
        }
        b0 = vol->fat_sector_buf[ent_offset];

        if (ent_offset == 511) {
            unsigned char next_buf[512];
            if (disk_read_drive(vol->drive_idx, vol->partition_lba + fat_sector + 1, next_buf) != 0) return 0x0FFFFFFF;
            b1 = next_buf[0];
        } else {
            b1 = vol->fat_sector_buf[ent_offset + 1];
        }

        unsigned short val = (unsigned short)b0 | (((unsigned short)b1) << 8);
        return (cluster & 1) ? ((val >> 4) & 0x0FFF) : (val & 0x0FFF);
    } else if (vol->type == 32) {
        unsigned int fat_offset = cluster * 4;
        unsigned int fat_sector = vol->reserved_sectors + (fat_offset / 512);
        unsigned int ent_offset = fat_offset % 512;
        if (vol->cached_fat_sector != fat_sector) {
            if (disk_read_drive(vol->drive_idx, vol->partition_lba + fat_sector, vol->fat_sector_buf) != 0) return 0x0FFFFFFF;
            vol->cached_fat_sector = fat_sector;
        }
        return (*(unsigned int*)&vol->fat_sector_buf[ent_offset]) & 0x0FFFFFFF;
    } else {
        unsigned int fat_offset = cluster * 2;
        unsigned int fat_sector = vol->reserved_sectors + (fat_offset / 512);
        unsigned int ent_offset = fat_offset % 512;
        if (vol->cached_fat_sector != fat_sector) {
            if (disk_read_drive(vol->drive_idx, vol->partition_lba + fat_sector, vol->fat_sector_buf) != 0) return 0x0FFFFFFF;
            vol->cached_fat_sector = fat_sector;
        }
        return *(unsigned short*)&vol->fat_sector_buf[ent_offset];
    }
}

static int fat_write_entry(fat_fs_t* vol, unsigned int cluster, unsigned int next_cluster) {
    unsigned int fat_offset = (vol->type == 32) ? (cluster * 4) : (cluster * 2);
    unsigned int fat_sector = vol->reserved_sectors + (fat_offset / 512);
    unsigned int ent_offset = fat_offset % 512;
    
    if (vol->cached_fat_sector != fat_sector) {
        if (disk_read_drive(vol->drive_idx, vol->partition_lba + fat_sector, vol->fat_sector_buf) != 0) return -1;
        vol->cached_fat_sector = fat_sector;
    }
    
    if (vol->type == 32) {
        unsigned int val = *(unsigned int*)&vol->fat_sector_buf[ent_offset];
        val = (val & 0xF0000000) | (next_cluster & 0x0FFFFFFF);
        *(unsigned int*)&vol->fat_sector_buf[ent_offset] = val;
    } else {
        *(unsigned short*)&vol->fat_sector_buf[ent_offset] = (unsigned short)next_cluster;
    }
    
    if (disk_write_drive(vol->drive_idx, vol->partition_lba + fat_sector, vol->fat_sector_buf) != 0) return -1;
    for (int i = 1; i < vol->num_fats; i++) {
        disk_write_drive(vol->drive_idx, vol->partition_lba + fat_sector + i * vol->sectors_per_fat, vol->fat_sector_buf);
    }
    return 0;
}

static unsigned int cluster_to_sector(fat_fs_t* vol, unsigned int cluster) {
    return vol->first_data_sector + (cluster - 2) * vol->sectors_per_cluster;
}

static unsigned int get_first_cluster(dir_entry_t* entry) {
    unsigned int cluster = entry->first_cluster;
    cluster |= ((unsigned int)entry->first_cluster_high) << 16;
    return cluster;
}
static void set_first_cluster(dir_entry_t* entry, unsigned int cluster) {
    entry->first_cluster = cluster & 0xFFFF;
    entry->first_cluster_high = (cluster >> 16) & 0xFFFF;
}

typedef int (*dir_callback_t)(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* user_data);

static int traverse_dir(fat_fs_t* vol, unsigned int start_cluster, dir_callback_t cb, void* user_data) {
    unsigned char buf[512];
    
    if (start_cluster == 0 && (vol->type == 12 || vol->type == 16)) {
        unsigned int root_start = vol->reserved_sectors + vol->num_fats * vol->sectors_per_fat;
        for (unsigned int i = 0; i < vol->root_dir_sectors; i++) {
            if (disk_read_drive(vol->drive_idx, vol->partition_lba + root_start + i, buf) != 0) return -1;
            for (int j = 0; j < 16; j++) {
                int ret = cb(vol, (dir_entry_t*)&buf[j * 32], root_start + i, j * 32, user_data);
                if (ret == 1) return 0;
                if (ret == 2) { disk_write_drive(vol->drive_idx, vol->partition_lba + root_start + i, buf); return 0; }
            }
        }
        return 0;
    }
    
    unsigned int current = start_cluster;
    if (current == 0 && vol->type == 32) current = vol->root_cluster;

    unsigned int end_cluster_mark = (vol->type == 12) ? 0x0FF8 : ((vol->type == 16) ? 0xFFF8 : 0x0FFFFFF8);
    
    while (current >= 2 && current < end_cluster_mark) {
        unsigned int start_sector = cluster_to_sector(vol, current);
        for (unsigned int i = 0; i < vol->sectors_per_cluster; i++) {
            if (disk_read_drive(vol->drive_idx, vol->partition_lba + start_sector + i, buf) != 0) return -1;
            for (int j = 0; j < 16; j++) {
                int ret = cb(vol, (dir_entry_t*)&buf[j * 32], start_sector + i, j * 32, user_data);
                if (ret == 1) return 0;
                if (ret == 2) { disk_write_drive(vol->drive_idx, vol->partition_lba + start_sector + i, buf); return 0; }
            }
        }
        unsigned int next = fat_read_entry(vol, current);
        if (next >= end_cluster_mark || next == 0) break;
        current = next;
    }
    return 0;
}

// ---------------------------------------------------------
// Find file in specific dir
// ---------------------------------------------------------
struct find_cb_data {
    const char* fat_name;
    const char* fat_ext;
    file_info_t* info;
    int found;
};

static int find_cb(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* ud) {
    (void)vol; (void)sector; (void)offset;
    if (entry->name[0] == 0x00) return 1;
    if (entry->name[0] == 0xE5 || entry->attr == 0x0F || (entry->attr & 0x08)) return 0;
    
    struct find_cb_data* data = (struct find_cb_data*)ud;
    if (str_casecmp((const char*)entry->name, data->fat_name, 8) == 0 &&
        str_casecmp((const char*)&entry->name[8], data->fat_ext, 3) == 0) {
        fat_name_to_str(entry->name, data->info->name);
        data->info->attr = entry->attr;
        data->info->size = entry->file_size;
        data->info->first_cluster = get_first_cluster(entry);
        data->found = 1;
        return 1;
    }
    return 0;
}

static int fat_find_in_dir(fat_fs_t* vol, unsigned int dir_cluster, const char* comp, file_info_t* info) {
    unsigned char fat_name[8], fat_ext[3];
    str_to_fat_name(comp, fat_name, fat_ext);
    struct find_cb_data data = { (const char*)fat_name, (const char*)fat_ext, info, 0 };
    if (traverse_dir(vol, dir_cluster, find_cb, &data) != 0) return -1;
    return data.found ? 0 : -1;
}

int fat_find(const char* name, file_info_t* info) {
    const char* path = name;
    fat_fs_t* vol = get_vol(&path);
    if (!vol) return -1;
    
    unsigned int cluster = 0;
    char comp[16];
    while (*path) {
        path = split_path(path, comp);
        if (comp[0] == 0) break;
        if (fat_find_in_dir(vol, cluster, comp, info) != 0) return -1;
        cluster = info->first_cluster;
    }
    return 0;
}

// ---------------------------------------------------------
// List specific dir
// ---------------------------------------------------------
struct list_cb_data {
    void (*callback)(const file_info_t*);
};

static int list_cb(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* ud) {
    (void)vol; (void)sector; (void)offset;
    if (entry->name[0] == 0x00) return 1; 
    if (entry->name[0] == 0xE5) return 0; 
    if (entry->attr == 0x0F) return 0;
    if (entry->attr & 0x08) return 0;
    
    struct list_cb_data* data = (struct list_cb_data*)ud;
    file_info_t info;
    fat_name_to_str(entry->name, info.name);
    info.attr = entry->attr;
    info.size = entry->file_size;
    info.first_cluster = get_first_cluster(entry);
    if (data->callback) data->callback(&info);
    return 0;
}

int fat_list_dir(const char* path, void (*callback)(const file_info_t*)) {
    fat_fs_t* vol = get_vol(&path);
    if (!vol) return -1;
    
    unsigned int cluster = 0;
    char comp[16];
    file_info_t info;
    
    while (*path) {
        path = split_path(path, comp);
        if (comp[0] == 0) break;
        if (fat_find_in_dir(vol, cluster, comp, &info) != 0) return -1;
        if (!(info.attr & 0x10)) return -1; // Not a directory
        cluster = info.first_cluster;
    }
    
    struct list_cb_data data = { callback };
    return traverse_dir(vol, cluster, list_cb, &data);
}

int fat_list_root(void (*callback)(const file_info_t*)) {
    return fat_list_dir("C:\\", callback);
}

file_handle_t* fat_open(const char* name) {
    file_info_t info;
    const char* path_for_vol = name;
    fat_fs_t* vol = get_vol(&path_for_vol);
    int vol_idx = vol ? (int)(vol - g_volumes) : 0;
    if (vol_idx < 0 || vol_idx >= MAX_VOLUMES) vol_idx = 0;

    if (fat_find(name, &info) != 0) return 0;
    if (info.attr & 0x10) return 0;
    str_copy(g_handle.name, info.name, 13);
    g_handle.size = info.size;
    g_handle.first_cluster = info.first_cluster;
    g_handle.current_cluster = info.first_cluster;
    g_handle.current_offset = 0;
    g_handle.cluster_offset = 0;
    g_handle.volume_idx = (unsigned char)vol_idx;
    g_handle.is_open = 1;
    return &g_handle;
}

int fat_read(file_handle_t* handle, unsigned char* buf, unsigned int size) {
    if (!handle || !handle->is_open) return -1;
    int v_idx = handle->volume_idx;
    if (v_idx < 0 || v_idx >= MAX_VOLUMES || g_volumes[v_idx].type == 0) v_idx = 0;
    fat_fs_t* vol = &g_volumes[v_idx];
    
    unsigned int bytes_read = 0, remaining = size;
    if (handle->current_offset >= handle->size) return 0;
    if (handle->current_offset + size > handle->size) remaining = handle->size - handle->current_offset;

    unsigned char sector_buf[512];
    while (remaining > 0) {
        if (handle->cluster_offset >= vol->sectors_per_cluster * 512) {
            unsigned int next = fat_read_entry(vol, handle->current_cluster);
            if (next >= 0x0FFFFFF8 || next == 0) break;
            handle->current_cluster = next;
            handle->cluster_offset = 0;
        }
        unsigned int sector = cluster_to_sector(vol, handle->current_cluster) + handle->cluster_offset / 512;
        unsigned int offset_in_sector = handle->cluster_offset % 512;
        if (disk_read_drive(vol->drive_idx, vol->partition_lba + sector, sector_buf) != 0) break;
        unsigned int to_read = 512 - offset_in_sector;
        if (to_read > remaining) to_read = remaining;
        for (unsigned int i = 0; i < to_read; i++) buf[bytes_read + i] = sector_buf[offset_in_sector + i];
        bytes_read += to_read;
        remaining -= to_read;
        handle->current_offset += to_read;
        handle->cluster_offset += to_read;
    }
    return bytes_read;
}

void fat_close(file_handle_t* handle) {
    if (handle) handle->is_open = 0;
}

static void free_chain(fat_fs_t* vol, unsigned int cluster) {
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        unsigned int next = fat_read_entry(vol, cluster);
        fat_write_entry(vol, cluster, 0);
        if (vol->is_free_calculated) vol->free_clusters++;
        cluster = next;
    }
}

// ---------------------------------------------------------
// Deletion
// ---------------------------------------------------------
struct del_cb_data {
    const char* fat_name;
    const char* fat_ext;
    int found;
};

static int del_cb(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* ud) {
    (void)sector; (void)offset;
    if (entry->name[0] == 0x00) return 1;
    if (entry->name[0] == 0xE5 || entry->attr == 0x0F || (entry->attr & 0x08)) return 0;
    
    struct del_cb_data* data = (struct del_cb_data*)ud;
    if (str_casecmp((const char*)entry->name, data->fat_name, 8) == 0 &&
        str_casecmp((const char*)&entry->name[8], data->fat_ext, 3) == 0) {
        
        unsigned int cluster = get_first_cluster(entry);
        entry->name[0] = 0xE5;
        if (!(entry->attr & 0x10)) {
            free_chain(vol, cluster);
        }
        
        data->found = 1;
        return 2; 
    }
    return 0;
}

int fat_delete(const char* name) {
    const char* path = name;
    fat_fs_t* vol = get_vol(&path);
    if (!vol) return -1;
    
    unsigned int cluster = 0;
    char comp[16];
    file_info_t info;
    
    // traverse to parent directory
    while (*path) {
        const char* next_path = split_path(path, comp);
        if (*next_path == 0) break; // last component
        if (fat_find_in_dir(vol, cluster, comp, &info) != 0) return -1;
        cluster = info.first_cluster;
        path = next_path;
    }
    
    unsigned char fat_name[8], fat_ext[3];
    str_to_fat_name(comp, fat_name, fat_ext);
    struct del_cb_data data = { (const char*)fat_name, (const char*)fat_ext, 0 };
    if (traverse_dir(vol, cluster, del_cb, &data) != 0) return -1;
    return data.found ? 0 : -1;
}

// ---------------------------------------------------------
// Rename
// ---------------------------------------------------------
struct ren_cb_data {
    const char* old_fat_name;
    const char* old_fat_ext;
    const char* new_fat_name;
    const char* new_fat_ext;
    int found;
};

static int ren_cb(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* ud) {
    (void)vol; (void)sector; (void)offset;
    if (entry->name[0] == 0x00) return 1;
    if (entry->name[0] == 0xE5 || entry->attr == 0x0F || (entry->attr & 0x08)) return 0;
    
    struct ren_cb_data* data = (struct ren_cb_data*)ud;
    if (str_casecmp((const char*)entry->name, data->old_fat_name, 8) == 0 &&
        str_casecmp((const char*)&entry->name[8], data->old_fat_ext, 3) == 0) {
        
        for (int k = 0; k < 8; k++) entry->name[k] = data->new_fat_name[k];
        for (int k = 0; k < 3; k++) entry->ext[k] = data->new_fat_ext[k];
        
        data->found = 1;
        return 2; 
    }
    return 0;
}

int fat_rename(const char* old_name, const char* new_name) {
    file_info_t dummy;
    if (fat_find(new_name, &dummy) == 0) return -2;

    const char* path = old_name;
    fat_fs_t* vol = get_vol(&path);
    if (!vol) return -1;
    
    unsigned int cluster = 0;
    char comp[16];
    while (*path) {
        const char* next_path = split_path(path, comp);
        if (*next_path == 0) break; 
        if (fat_find_in_dir(vol, cluster, comp, &dummy) != 0) return -1;
        cluster = dummy.first_cluster;
        path = next_path;
    }
    
    const char* new_path = new_name;
    char new_comp[16];
    while (*new_path) {
        const char* next_path = split_path(new_path, new_comp);
        if (*next_path == 0) break; 
        new_path = next_path;
    }

    unsigned char old_fat_name[8], old_fat_ext[3];
    unsigned char new_fat_name[8], new_fat_ext[3];
    str_to_fat_name(comp, old_fat_name, old_fat_ext);
    str_to_fat_name(new_comp, new_fat_name, new_fat_ext);

    struct ren_cb_data data = { (const char*)old_fat_name, (const char*)old_fat_ext, (const char*)new_fat_name, (const char*)new_fat_ext, 0 };
    if (traverse_dir(vol, cluster, ren_cb, &data) != 0) return -1;
    return data.found ? 0 : -1;
}

// ---------------------------------------------------------
// Creation
// ---------------------------------------------------------
static unsigned int allocate_cluster(fat_fs_t* vol) {
    for (unsigned int i = 2; i < vol->total_clusters + 2; i++) {
        if (fat_read_entry(vol, i) == 0) {
            fat_write_entry(vol, i, 0x0FFFFFFF);
            if (vol->is_free_calculated && vol->free_clusters > 0) vol->free_clusters--;
            return i;
        }
    }
    return 0;
}

int fat_get_volume_space(char letter, unsigned int* total_kb, unsigned int* free_kb) {
    char path_buf[4] = { letter, ':', '\\', 0 };
    const char* p = path_buf;
    fat_fs_t* vol = get_vol(&p);
    if (!vol || vol->type == 0) return -1;

    unsigned int total_k = (unsigned int)(((unsigned long long)vol->total_clusters * vol->sectors_per_cluster * 512) / 1024);
    if (total_kb) *total_kb = total_k;

    if (vol->is_free_calculated) {
        if (free_kb) *free_kb = (unsigned int)(((unsigned long long)vol->free_clusters * vol->sectors_per_cluster * 512) / 1024);
        return 0;
    }

    // Try reading FSInfo sector if FAT32
    if (vol->type == 32) {
        unsigned char fsinfo[512];
        if (disk_read_drive(vol->drive_idx, vol->partition_lba + 1, fsinfo) == 0) {
            unsigned int lead_sig = *(unsigned int*)&fsinfo[0];
            unsigned int struct_sig = *(unsigned int*)&fsinfo[484];
            unsigned int free_count = *(unsigned int*)&fsinfo[488];
            if (lead_sig == 0x41615252 && struct_sig == 0x61417272 && free_count != 0xFFFFFFFF && free_count <= vol->total_clusters) {
                vol->free_clusters = free_count;
                vol->is_free_calculated = 1;
                if (free_kb) *free_kb = (unsigned int)(((unsigned long long)free_count * vol->sectors_per_cluster * 512) / 1024);
                return 0;
            }
        }
    }

    // Otherwise scan FAT table to count 0x00 entries
    unsigned int free_cnt = 0;
    for (unsigned int i = 2; i < vol->total_clusters + 2; i++) {
        if (fat_read_entry(vol, i) == 0) free_cnt++;
    }
    vol->free_clusters = free_cnt;
    vol->is_free_calculated = 1;
    if (free_kb) *free_kb = (unsigned int)(((unsigned long long)free_cnt * vol->sectors_per_cluster * 512) / 1024);
    return 0;
}

struct cre_cb_data {
    const char* fat_name;
    const char* fat_ext;
    int is_dir;
    unsigned int parent_cluster;
    int found;
};

static int cre_cb(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* ud) {
    (void)vol; (void)sector; (void)offset;
    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        struct cre_cb_data* data = (struct cre_cb_data*)ud;
        
        unsigned char* p = (unsigned char*)entry;
        for (int k = 0; k < 32; k++) p[k] = 0;
        
        for (int k = 0; k < 8; k++) entry->name[k] = data->fat_name[k];
        for (int k = 0; k < 3; k++) entry->ext[k] = data->fat_ext[k];
        
        entry->attr = data->is_dir ? 0x10 : 0x20;
        unsigned int new_cluster = 0;
        if (data->is_dir) {
            new_cluster = allocate_cluster(vol);
            unsigned char sector_buf[512] = {0};

            // '.' entry pointing to self
            dir_entry_t* dot = (dir_entry_t*)sector_buf;
            for (int k = 0; k < 8; k++) dot->name[k] = ' ';
            for (int k = 0; k < 3; k++) dot->ext[k] = ' ';
            dot->name[0] = '.';
            dot->attr = 0x10;
            set_first_cluster(dot, new_cluster);
            dot->file_size = 0;

            // '..' entry pointing to parent directory
            dir_entry_t* dotdot = (dir_entry_t*)(sector_buf + sizeof(dir_entry_t));
            for (int k = 0; k < 8; k++) dotdot->name[k] = ' ';
            for (int k = 0; k < 3; k++) dotdot->ext[k] = ' ';
            dotdot->name[0] = '.';
            dotdot->name[1] = '.';
            dotdot->attr = 0x10;
            set_first_cluster(dotdot, data->parent_cluster);
            dotdot->file_size = 0;

            disk_write_drive(vol->drive_idx, vol->partition_lba + cluster_to_sector(vol, new_cluster), sector_buf);

            unsigned char zero[512] = {0};
            for (unsigned int i = 1; i < vol->sectors_per_cluster; i++) {
                disk_write_drive(vol->drive_idx, vol->partition_lba + cluster_to_sector(vol, new_cluster) + i, zero);
            }
        }
        set_first_cluster(entry, new_cluster);
        entry->file_size = 0;
        
        data->found = 1;
        return 2; 
    }
    return 0;
}

int fat_create_ext(const char* name, int is_dir) {
    file_info_t dummy;
    if (fat_find(name, &dummy) == 0) return -2;

    const char* path = name;
    fat_fs_t* vol = get_vol(&path);
    if (!vol) return -1;
    
    unsigned int cluster = 0;
    char comp[16];
    while (*path) {
        const char* next_path = split_path(path, comp);
        if (*next_path == 0) break;
        if (fat_find_in_dir(vol, cluster, comp, &dummy) != 0) return -1;
        cluster = dummy.first_cluster;
        path = next_path;
    }

    unsigned char fat_name[8], fat_ext[3];
    str_to_fat_name(comp, fat_name, fat_ext);

    struct cre_cb_data data = { (const char*)fat_name, (const char*)fat_ext, is_dir, cluster, 0 };
    if (traverse_dir(vol, cluster, cre_cb, &data) != 0) return -1;
    return data.found ? 0 : -3;
}

int fat_create(const char* name) { return fat_create_ext(name, 0); }
int fat_create_dir(const char* name) { return fat_create_ext(name, 1); }

struct update_cb_data {
    const char* fat_name;
    const char* fat_ext;
    unsigned int size;
    unsigned int first_cluster;
    int found;
};

static int update_cb(fat_fs_t* vol, dir_entry_t* entry, unsigned int sector, unsigned int offset, void* ud) {
    (void)vol; (void)sector; (void)offset;
    if (entry->name[0] == 0x00) return 1;
    if (entry->name[0] == 0xE5 || entry->attr == 0x0F || (entry->attr & 0x08)) return 0;
    
    struct update_cb_data* data = (struct update_cb_data*)ud;
    if (str_casecmp((const char*)entry->name, data->fat_name, 8) == 0 &&
        str_casecmp((const char*)&entry->name[8], data->fat_ext, 3) == 0) {
        
        entry->file_size = data->size;
        set_first_cluster(entry, data->first_cluster);
        data->found = 1;
        return 2; 
    }
    return 0;
}

int fat_write_file(const char* name, const unsigned char* buf, unsigned int size) {
    file_info_t info;
    if (fat_find(name, &info) == 0) {
        const char* path = name;
        fat_fs_t* vol = get_vol(&path);
        free_chain(vol, info.first_cluster);
    } else {
        if (fat_create(name) != 0) return -1;
    }
    
    const char* path = name;
    fat_fs_t* vol = get_vol(&path);
    unsigned int cluster = 0;
    char comp[16];
    while (*path) {
        const char* next_path = split_path(path, comp);
        if (*next_path == 0) break;
        if (fat_find_in_dir(vol, cluster, comp, &info) != 0) return -1;
        cluster = info.first_cluster;
        path = next_path;
    }
    
    unsigned int first_cluster = 0;
    if (size > 0) {
        unsigned int current_cluster = 0;
        unsigned int remaining = size;
        unsigned int offset = 0;
        unsigned char sector_buf[512];
        
        while (remaining > 0) {
            unsigned int new_cluster = allocate_cluster(vol);
            if (new_cluster == 0) return -1; 
            if (first_cluster == 0) first_cluster = new_cluster;
            
            if (current_cluster != 0) {
                fat_write_entry(vol, current_cluster, new_cluster);
            }
            current_cluster = new_cluster;
            
            for (unsigned int i = 0; i < vol->sectors_per_cluster && remaining > 0; i++) {
                unsigned int to_write = remaining > 512 ? 512 : remaining;
                for (unsigned int j = 0; j < 512; j++) {
                    sector_buf[j] = (j < to_write) ? buf[offset + j] : 0;
                }
                disk_write_drive(vol->drive_idx, vol->partition_lba + cluster_to_sector(vol, new_cluster) + i, sector_buf);
                remaining -= to_write;
                offset += to_write;
            }
        }
    }
    
    unsigned char fat_name[8], fat_ext[3];
    str_to_fat_name(comp, fat_name, fat_ext);
    struct update_cb_data data = { (const char*)fat_name, (const char*)fat_ext, size, first_cluster, 0 };
    traverse_dir(vol, cluster, update_cb, &data);
    
    return 0;
}

extern void disk_flush_all(void);
void fat_flush_all(void) {
    disk_flush_all();
}