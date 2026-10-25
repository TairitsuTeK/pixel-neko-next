#ifndef CT4810_H
#define CT4810_H

// Creative CT4810 / CT5880 / Ensoniq ES1371 / ES1373 PCI Audio Driver

#define ENSONIQ_VENDOR_ID       0x1274
#define ES1371_DEV_ID           0x1371
#define CT5880_DEV_ID           0x5880
#define CREATIVE_VENDOR_ID      0x1102

// ES1371 / CT5880 Registers (BAR0 offsets)
#define ES_REG_CONTROL          0x00 // Control register
#define ES_REG_STATUS           0x04 // Status register
#define ES_REG_UART_DATA        0x08 // UART Data
#define ES_REG_UART_STATUS      0x09 // UART Status
#define ES_REG_MEMPAGE          0x0C // Memory Page
#define ES_REG_SRC              0x10 // Sample Rate Converter
#define ES_REG_CODEC            0x14 // AC'97 Codec Port
#define ES_REG_LEGACY           0x18 // Legacy Control
#define ES_REG_CH_FORMAT        0x20 // Channel Format Register

// DAC2 / Playback Frame Registers
#define ES_REG_DAC2_FRAME1      0x38 // DAC2 Buffer Base Address
#define ES_REG_DAC2_FRAME2      0x3C // DAC2 Sample Count / Size

// Control Register (0x00) Bits
#define ES_CTL_DAC2_EN          0x00000020 // DAC2 Enable (Bit 5 in ES1371)
#define ES_CTL_DAC1_EN          0x00000010 // DAC1 Enable (Bit 4 in ES1371)
#define ES_CTL_ADC_EN           0x00000008 // ADC Enable
#define ES_CTL_CCB_INTRM        0x00000400 // CCB Interrupt Mask
#define ES_CTL_BREQ             0x00000001 // Bus Master Request

// Status Register (0x04) Bits
#define ES_STAT_INTR            0x80000000 // Interrupt Pending
#define ES_STAT_CSTAT           0x00000400 // Codec Status: 1 = ready
#define ES_STAT_DAC2_INT        0x00000004 // DAC2 Interrupt
#define ES_STAT_DAC1_INT        0x00000002 // DAC1 Interrupt

// AC'97 Codec Status & Control (0x14)
#define ES_CODEC_BUSY           0x40000000 // Bit 30: Codec Busy / WIP
#define ES_CODEC_READ           0x00800000 // Bit 23: 1 = Read, 0 = Write

// DMA Ring Buffer Dimensions: 64KB total, two 32KB halves
#define CT_DMA_BUFFER_BYTES     65536
#define CT_DMA_HALF_BYTES       32768
#define CT_DMA_TOTAL_FRAMES     16384 // in 16-bit stereo (4 bytes/frame)
#define CT_DMA_HALF_FRAMES      8192

int ct4810_init(void);
int ct4810_is_present(void);
const char* ct4810_get_name(void);
unsigned short ct4810_get_io_base(void);

void ct4810_set_format(int rate, int bits, int stereo);
void ct4810_start_dma(void);
void ct4810_stop_dma(void);
void ct4810_pause_dma(int pause);
void ct4810_set_volume(int master_vol, int pcm_vol);

unsigned int ct4810_get_dma_frame_pos(void);
const void* ct4810_get_dma_buffer(void);
void ct4810_write_half(int half_idx, const void* data, unsigned int bytes);
void ct4810_silence_half(int half_idx);
int ct4810_poll_half_needed(void);
void ct4810_mark_half_filled(int half_idx);

#endif // CT4810_H
