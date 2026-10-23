#ifndef PORT_H
#define PORT_H

#ifndef NULL
#define NULL ((void*)0)
#endif

static inline void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(unsigned short port, unsigned short value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(unsigned short port, unsigned int value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned int pci_read_config(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset)
{
    unsigned int address = (1U << 31) | ((unsigned int)bus << 16) | ((unsigned int)slot << 11) | ((unsigned int)func << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static inline void pci_write_config(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset, unsigned int val)
{
    unsigned int address = (1U << 31) | ((unsigned int)bus << 16) | ((unsigned int)slot << 11) | ((unsigned int)func << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    outl(0xCFC, val);
}

#endif