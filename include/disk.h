#ifndef DISK_H
#define DISK_H

#define MAX_DRIVES 8

typedef struct {
    int present;
    int is_atapi;
    unsigned int base_port;
    unsigned int ctrl_port;
    int drive_idx;
    unsigned int lba_offset;
    unsigned int total_sectors;
} disk_drive_t;

extern disk_drive_t g_drives[MAX_DRIVES];

int disk_detect_all(void);
int disk_read_drive(int drive, unsigned int lba, unsigned char* buf);
int disk_write_drive(int drive, unsigned int lba, const unsigned char* buf);

void disk_set_grub_mode(int mode);
int disk_get_grub_mode(void);

void disk_flush(int drive);
void disk_flush_all(void);

// For backwards compatibility where drive 0 is implicit
int disk_detect(void);
int disk_read(unsigned int lba, unsigned char* buf);
int disk_write(unsigned int lba, const unsigned char* buf);

#endif