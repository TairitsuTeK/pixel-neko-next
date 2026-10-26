#include "dma.h"
#include "mm.h"
#include "mmio.h"

#define DMA_PAGE_SIZE 4096U

static void dma_zero(void* address, unsigned int bytes)
{
    unsigned char* p = (unsigned char*)address;
    for (unsigned int i = 0; i < bytes; i++) p[i] = 0;
}

dma_buffer_t dma_alloc_aligned(unsigned int bytes, unsigned int alignment)
{
    dma_buffer_t buffer;
    buffer.virt = 0;
    buffer.phys = 0;
    buffer.bytes = 0;
    buffer.pages = 0;
    buffer.alignment = alignment;
    buffer.owned = 0;

    if (bytes == 0) return buffer;
    if (alignment < DMA_PAGE_SIZE) alignment = DMA_PAGE_SIZE;
    if (alignment & (alignment - 1U)) {
        unsigned int rounded = DMA_PAGE_SIZE;
        while (rounded < alignment) rounded <<= 1U;
        alignment = rounded;
    }
    if (alignment % DMA_PAGE_SIZE) return buffer;

    unsigned int pages = (bytes + DMA_PAGE_SIZE - 1U) / DMA_PAGE_SIZE;
    unsigned int align_pages = alignment / DMA_PAGE_SIZE;
    void* memory = pmm_alloc_pages_aligned(pages, align_pages);
    if (!memory) return buffer;

    dma_zero(memory, pages * DMA_PAGE_SIZE);
    buffer.virt = memory;
    buffer.phys = (unsigned int)memory;
    buffer.bytes = bytes;
    buffer.pages = pages;
    buffer.alignment = alignment;
    buffer.owned = 1;
    return buffer;
}

void dma_free(dma_buffer_t* buffer)
{
    if (!buffer || !buffer->owned || !buffer->virt) return;
    pmm_free_pages(buffer->virt, buffer->pages);
    buffer->virt = 0;
    buffer->phys = 0;
    buffer->bytes = 0;
    buffer->pages = 0;
    buffer->owned = 0;
}

void dma_flush(const dma_buffer_t* buffer)
{
    if (!buffer || !buffer->virt) return;
    mmio_barrier();
}

int dma_is_valid(const dma_buffer_t* buffer)
{
    return buffer && buffer->owned && buffer->virt &&
           buffer->phys == (unsigned int)buffer->virt && buffer->bytes != 0;
}
