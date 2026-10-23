#include "mm.h"

unsigned int total_ram_mb = 0;
unsigned int usable_ram_mb = 0;

#define PAGE_SIZE 4096
#define MAX_PAGES (0x100000000ULL / PAGE_SIZE)
static unsigned char pmm_bitmap[MAX_PAGES / 8];
static unsigned int next_free_page = 0;

struct multiboot_mmap_entry {
    unsigned int size;
    unsigned int addr_low;
    unsigned int addr_high;
    unsigned int len_low;
    unsigned int len_high;
    unsigned int type;
} __attribute__((packed));

void pmm_init(void) {
    // Initialize bitmap: mark all as used initially
    for (unsigned int i = 0; i < sizeof(pmm_bitmap); i++) {
        pmm_bitmap[i] = 0xFF;
    }
    
    // 3. Parse multiboot info
    unsigned int mbi_addr = *(unsigned int*)0x7F64;
    if (mbi_addr == 0) return; // Fallback
    
    unsigned int flags = *(unsigned int*)mbi_addr;
    
    if (flags & (1 << 0)) {
        unsigned int mem_lower = *(unsigned int*)(mbi_addr + 4);
        unsigned int mem_upper = *(unsigned int*)(mbi_addr + 8);
        total_ram_mb = (mem_lower + mem_upper) / 1024 + 1;
    }
    
    if (flags & (1 << 6)) {
        unsigned int mmap_length = *(unsigned int*)(mbi_addr + 44);
        unsigned int mmap_addr = *(unsigned int*)(mbi_addr + 48);
        
        struct multiboot_mmap_entry* entry = (struct multiboot_mmap_entry*)mmap_addr;
        unsigned int usable_bytes = 0;
        
        while ((unsigned int)entry < mmap_addr + mmap_length) {
            if (entry->type == 1) { // 1 = Available RAM
                unsigned long long start = ((unsigned long long)entry->addr_high << 32) | entry->addr_low;
                unsigned long long len = ((unsigned long long)entry->len_high << 32) | entry->len_low;
                
                // Clamp to 4GB for 32-bit system without PAE
                if (start >= 0x100000000ULL) {
                    entry = (struct multiboot_mmap_entry*)((unsigned int)entry + entry->size + 4);
                    continue;
                }
                if (start + len > 0x100000000ULL) {
                    len = 0x100000000ULL - start;
                }
                
                usable_bytes += (unsigned int)len;
                
                unsigned int start_page = (unsigned int)(start / PAGE_SIZE);
                unsigned int end_page = (unsigned int)((start + len) / PAGE_SIZE);
                
                for (unsigned int p = start_page; p < end_page && p < MAX_PAGES; p++) {
                    pmm_bitmap[p / 8] &= ~(1 << (p % 8));
                }
            }
            entry = (struct multiboot_mmap_entry*)((unsigned int)entry + entry->size + 4);
        }
        usable_ram_mb = usable_bytes / (1024 * 1024);
        
        // Let's also sync total_ram_mb to be more accurate if usable is around the same
        if (usable_ram_mb > total_ram_mb) {
            total_ram_mb = usable_ram_mb;
        }
    }
    
    // Reserve first 4MB for safety (kernel, vbe, etc)
    for (unsigned int p = 0; p < (0x400000 / PAGE_SIZE); p++) {
        pmm_bitmap[p / 8] |= (1 << (p % 8));
    }
}

void* pmm_alloc_page(void) {
    for (unsigned int i = next_free_page; i < MAX_PAGES; i++) {
        if (pmm_bitmap[i / 8] != 0xFF) {
            if ((pmm_bitmap[i / 8] & (1 << (i % 8))) == 0) {
                pmm_bitmap[i / 8] |= (1 << (i % 8));
                next_free_page = i + 1;
                return (void*)(i * PAGE_SIZE);
            }
        }
    }
    return 0; // Out of memory
}

void pmm_free_page(void* ptr) {
    unsigned int p = (unsigned int)ptr / PAGE_SIZE;
    if (p < MAX_PAGES) {
        pmm_bitmap[p / 8] &= ~(1 << (p % 8));
        if (p < next_free_page) {
            next_free_page = p;
        }
    }
}
