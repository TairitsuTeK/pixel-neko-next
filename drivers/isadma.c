#include "isadma.h"
#include "port.h"

#define DMA_CHANNEL_2_MASK   0x0A
#define DMA_CHANNEL_2_MODE   0x0B
#define DMA_FLIP_FLOP        0x0C
#define DMA_PAGE_PORT_2      0x81
#define DMA_ADDR_PORT_2      0x04
#define DMA_COUNT_PORT_2      0x05

void isadma_setup_read(void* buffer, unsigned int length) {
    unsigned int addr = (unsigned int)buffer;
    unsigned int count = length - 1;
    unsigned char page = (unsigned char)((addr >> 16) & 0xFF);
    unsigned short offset = (unsigned short)(addr & 0xFFFF);

    // Mask channel 2
    outb(DMA_CHANNEL_2_MASK, 0x06); // 0x04 | 2

    // Single mode, address increment, auto-init off, read transfer (device to memory: 0x44 | 2 = 0x46)
    outb(DMA_CHANNEL_2_MODE, 0x46);

    // Reset flip-flop
    outb(DMA_FLIP_FLOP, 0xFF);

    // Send address offset (low then high)
    outb(DMA_ADDR_PORT_2, (unsigned char)(offset & 0xFF));
    outb(DMA_ADDR_PORT_2, (unsigned char)((offset >> 8) & 0xFF));

    // Send page
    outb(DMA_PAGE_PORT_2, page);

    // Reset flip-flop
    outb(DMA_FLIP_FLOP, 0xFF);

    // Send transfer count (low then high)
    outb(DMA_COUNT_PORT_2, (unsigned char)(count & 0xFF));
    outb(DMA_COUNT_PORT_2, (unsigned char)((count >> 8) & 0xFF));

    // Unmask channel 2
    outb(DMA_CHANNEL_2_MASK, 0x02);
}

void isadma_setup_write(const void* buffer, unsigned int length) {
    unsigned int addr = (unsigned int)buffer;
    unsigned int count = length - 1;
    unsigned char page = (unsigned char)((addr >> 16) & 0xFF);
    unsigned short offset = (unsigned short)(addr & 0xFFFF);

    outb(DMA_CHANNEL_2_MASK, 0x06);
    // Write transfer (memory to device: 0x48 | 2 = 0x4A)
    outb(DMA_CHANNEL_2_MODE, 0x4A);
    outb(DMA_FLIP_FLOP, 0xFF);
    outb(DMA_ADDR_PORT_2, (unsigned char)(offset & 0xFF));
    outb(DMA_ADDR_PORT_2, (unsigned char)((offset >> 8) & 0xFF));
    outb(DMA_PAGE_PORT_2, page);
    outb(DMA_FLIP_FLOP, 0xFF);
    outb(DMA_COUNT_PORT_2, (unsigned char)(count & 0xFF));
    outb(DMA_COUNT_PORT_2, (unsigned char)((count >> 8) & 0xFF));
    outb(DMA_CHANNEL_2_MASK, 0x02);
}
