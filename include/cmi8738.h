#ifndef CMI8738_H
#define CMI8738_H

// C-Media CMI8738 / CMI8338 PCI Audio Controller Driver

#define CMI_VENDOR_ID          0x13F6
#define CMI_DEV_8738           0x0111
#define CMI_DEV_8338A          0x0100
#define CMI_DEV_8338B          0x0101

// I/O Registers (BAR0 offsets)
#define CMI_REG_FUNCTRL0       0x00
#define CMI_REG_FUNCTRL1       0x04
#define CMI_REG_CHFORMAT       0x08
#define CMI_REG_INT_HLDCLR     0x0C
#define CMI_REG_INT_STATUS     0x10
#define CMI_REG_LEGACY_CTRL    0x14
#define CMI_REG_MISC_CTRL      0x18
#define CMI_REG_SB_INDEX       0x22
#define CMI_REG_SB_DATA        0x23
#define CMI_REG_CH0_FRAME1     0x80
#define CMI_REG_CH0_FRAME2     0x84

// DMA Ring Buffer Dimensions: 64KB total, two 32KB halves
#define CMI_DMA_BUFFER_BYTES   65536
#define CMI_DMA_HALF_BYTES     32768
#define CMI_DMA_TOTAL_FRAMES   16384 // in 16-bit stereo (4 bytes/frame)
#define CMI_DMA_HALF_FRAMES    8192

int cmi8738_init(void);
int cmi8738_is_present(void);
const char* cmi8738_get_name(void);
unsigned short cmi8738_get_io_base(void);

// Format & DMA Controls
void cmi8738_set_format(int rate, int bits, int stereo);
void cmi8738_start_dma(void);
void cmi8738_stop_dma(void);
void cmi8738_pause_dma(int pause);
void cmi8738_set_volume(int master_vol, int pcm_vol); // 0 ~ 100

// Query current playback position in DMA ring buffer (sample frame offset 0..16383)
unsigned int cmi8738_get_dma_frame_pos(void);

// Buffer write operations: half_idx: 0 = first 32KB, 1 = second 32KB
void cmi8738_write_half(int half_idx, const void* data, unsigned int bytes);
void cmi8738_silence_half(int half_idx);

// Poll DMA status: returns 0 if half 0 needs filling, 1 if half 1 needs filling, -1 if no half needed
int cmi8738_poll_half_needed(void);
void cmi8738_mark_half_filled(int half_idx);

#endif // CMI8738_H
