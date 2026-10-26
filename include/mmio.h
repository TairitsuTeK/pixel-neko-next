#ifndef MMIO_H
#define MMIO_H

static inline void mmio_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline unsigned char mmio_read8(const volatile void* address)
{
    return *(volatile const unsigned char*)address;
}

static inline unsigned short mmio_read16(const volatile void* address)
{
    return *(volatile const unsigned short*)address;
}

static inline unsigned int mmio_read32(const volatile void* address)
{
    return *(volatile const unsigned int*)address;
}

static inline void mmio_write8(volatile void* address, unsigned char value)
{
    *(volatile unsigned char*)address = value;
}

static inline void mmio_write16(volatile void* address, unsigned short value)
{
    *(volatile unsigned short*)address = value;
}

static inline void mmio_write32(volatile void* address, unsigned int value)
{
    *(volatile unsigned int*)address = value;
}

#endif
