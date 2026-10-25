#ifndef CT4810_H
#define CT4810_H

// Creative CT4810 / CT5880 / Ensoniq ES1371 / ES1373 PCI Audio Driver

#define ENSONIQ_VENDOR_ID       0x1274
#define ES1371_DEV_ID           0x1371
#define CT5880_DEV_ID           0x5880
#define CREATIVE_VENDOR_ID      0x1102
#define EV1938_DEV_ID           0x8938

// ES1371 / CT5880 Registers (BAR0 offsets)
#define ES_REG_CONTROL          0x00 // Control register
#define ES_REG_STATUS           0x04 // Status register
#define ES_REG_UART_DATA        0x08 // UART Data
#define ES_REG_UART_STATUS      0x09 // UART Status
#define ES_REG_MEMPAGE          0x0C // Memory Page
#define ES_REG_SRC              0x10 // Sample Rate Converter
#define ES_REG_CODEC            0x14 // AC'97 Codec Port
#define ES_REG_LEGACY           0x18 // Legacy Control
#define ES_REG_SERIAL           0x20 // Serial Control / Channel Format
#define ES_REG_DAC2_COUNT       0x28 // DAC2 Sample / Period Count

// Registers mapped under PAGE 0x0C (ES_PAGE_DAC)
#define ES_PAGE_DAC             0x0C
#define ES_REG_DAC2_FRAME1      0x38 // DAC2 Buffer Base Physical Address
#define ES_REG_DAC2_FRAME2      0x3C // DAC2 Current Count (31:16) & Frame Size (15:0)

// Control Register (0x00) Bits
#define ES_CTL_BREQ             0x00000001 // Bus Master Request
#define ES_CTL_ADC_EN           0x00000010 // ADC Enable (Bit 4)
#define ES_CTL_DAC2_EN          0x00000020 // DAC2 Enable (Bit 5)
#define ES_CTL_DAC1_EN          0x00000040 // DAC1 Enable (Bit 6)

// Serial Control Register (0x20) Bits
#define ES_P2_MODEO(o)          (((o) & 0x03) << 2) // Mode: 0=8M, 1=8S, 2=16M, 3=16S
#define ES_P2_MODEM             (0x03 << 2)
#define ES_P2_INT_EN            (1 << 9)
#define ES_P2_PAUSE             (1 << 12)
#define ES_P2_LOOP_SEL          (1 << 14) // 0 = Loop mode, 1 = Stop mode

// Sample Rate Converter (0x10) Bits & Masks
#define ES_1371_DIS_R1          (1 << 19)
#define ES_1371_DIS_P2          (1 << 20)
#define ES_1371_DIS_P1          (1 << 21)
#define ES_1371_SRC_DISABLE     (1 << 22)
#define ES_1371_SRC_RAM_BUSY    (1 << 23)
#define ES_1371_SRC_RAM_WE      (1 << 24)

// AC'97 Codec Port (0x14) Bits
#define ES_1371_CODEC_WIP       (1 << 30) // Codec Write/Access In Progress
#define ES_1371_CODEC_PORD      (1 << 31) // Codec Read In Progress / Status

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
