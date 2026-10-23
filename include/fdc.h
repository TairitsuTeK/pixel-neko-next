#ifndef FDC_H
#define FDC_H

#include "blkdev.h"

int  fdc_init(void);
int  fdc_has_drive(int drive);
int  fdc_read_sectors(int drive, unsigned int lba, unsigned int count, unsigned char* buf);
int  fdc_write_sectors(int drive, unsigned int lba, unsigned int count, const unsigned char* buf);
media_state_t fdc_check_media(int drive);
unsigned int fdc_get_total_sectors(int drive);

#endif
