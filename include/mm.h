#ifndef MM_H
#define MM_H

#include "cpu.h"

extern unsigned int total_ram_mb;
extern unsigned int usable_ram_mb;

void pmm_init(void);

// Basic PMM API
void* pmm_alloc_page(void);
void pmm_free_page(void* ptr);

void* pmm_alloc_pages(unsigned int count);
void  pmm_free_pages(void* ptr, unsigned int count);
unsigned int pmm_get_free_pages(void);
unsigned int pmm_get_free_mb(void);

#endif // MM_H
