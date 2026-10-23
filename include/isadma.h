#ifndef ISADMA_H
#define ISADMA_H

// ISA DMA channel 2 setup for floppy drive
// buffer must be below 16MB and not cross a 64KB boundary
void isadma_setup_read(void* buffer, unsigned int length);
void isadma_setup_write(const void* buffer, unsigned int length);

#endif
