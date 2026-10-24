#ifndef FAT_H
#define FAT_H

typedef struct {
    unsigned short bytes_per_sector;
    unsigned char  sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char  num_fats;
    unsigned short root_entries;
    unsigned short total_sectors_16;
    unsigned char  media_type;
    unsigned short sectors_per_fat;
    unsigned short sectors_per_track;
    unsigned short num_heads;
    unsigned int   hidden_sectors;
    unsigned int   total_sectors_32;
    unsigned int   sectors_per_fat_32;
    unsigned short ext_flags;
    unsigned short fs_version;
    unsigned int   root_cluster;
} __attribute__((packed)) bpb_t;

typedef struct {
    unsigned char  name[8];
    unsigned char  ext[3];
    unsigned char  attr;
    unsigned char  reserved;
    unsigned char  create_time_tenth;
    unsigned short create_time;
    unsigned short create_date;
    unsigned short access_date;
    unsigned short first_cluster_high;
    unsigned short write_time;
    unsigned short write_date;
    unsigned short first_cluster;
    unsigned int   file_size;
} __attribute__((packed)) dir_entry_t;

typedef struct {
    unsigned char  type; // 16 or 32
    unsigned char  sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char  num_fats;
    unsigned short root_entries;
    unsigned int   sectors_per_fat;
    unsigned int   root_dir_sectors;
    unsigned int   first_data_sector;
    unsigned int   total_clusters;
    unsigned int   root_cluster;
    
    int            drive_idx;
    unsigned int   partition_lba;
    
    unsigned int   cached_fat_sector;
    unsigned char  fat_sector_buf[512];

    unsigned int   free_clusters;
    int            is_free_calculated;
} fat_fs_t;

typedef struct {
    char           name[32];
    unsigned char  attr;
    unsigned int   size;
    unsigned int   first_cluster;
} file_info_t;

typedef struct {
    char           name[13];
    unsigned int   size;
    unsigned int   first_cluster;
    unsigned int   current_cluster;
    unsigned int   current_offset;
    unsigned int   cluster_offset;
    unsigned char  volume_idx;
    unsigned char  is_open;
} file_handle_t;

int fat_init(void);
int fat_list_root(void (*callback)(const file_info_t*));
int fat_list_dir(const char* path, void (*callback)(const file_info_t*));
int fat_find(const char* name, file_info_t* info);
file_handle_t* fat_open(const char* name);
int fat_read(file_handle_t* handle, unsigned char* buf, unsigned int size);
int fat_seek(file_handle_t* handle, unsigned int offset);
void fat_close(file_handle_t* handle);
int fat_delete(const char* name);
int fat_rename(const char* old_name, const char* new_name);
int fat_create(const char* name);
int fat_create_dir(const char* name);
int fat_write_file(const char* name, const unsigned char* buf, unsigned int size);
void fat_flush_all(void);

int fat_mount_volume(int vol_idx, int drive_idx, unsigned int lba);
int mount_fat(fat_fs_t* vol, int drive_idx, unsigned int lba, unsigned char* boot_sector);
int fat_get_volume_space(char letter, unsigned int* total_kb, unsigned int* free_kb);

#define MAX_VOLUMES 16
extern fat_fs_t g_volumes[MAX_VOLUMES];

#endif