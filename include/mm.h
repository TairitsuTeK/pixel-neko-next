#ifndef MM_H
#define MM_H

#include "cpu.h"

extern unsigned int total_ram_mb;
extern unsigned int usable_ram_mb;

void pmm_init(void);

// Basic PMM API
void* pmm_alloc_page(void);
void pmm_free_page(void* ptr);

#endif // MM_H
