#ifndef CT4810_H
#define CT4810_H

// Creative CT4810 / CT5880 / Ensoniq ES1371 / ES1373 PCI Audio Driver

#define ENSONIQ_VENDOR_ID       0x1274
#define ES1371_DEV_ID           0x1371
#define ES1371_COMPAT_DEV_ID    0x0371
#define CT5880_DEV_ID           0x5880
#define CREATIVE_VENDOR_ID      0x1102
#define EV1938_DEV_ID           0x8938

// ES1371 / CT5880 direct I/O registers (BAR0 offsets).
#define ES_REG_CONTROL          0x00
#define ES_REG_STATUS           0x04
#define ES_REG_MEMPAGE          0x0C
#define ES_REG_SRC              0x10
#define ES_REG_CODEC            0x14
#define ES_REG_LEGACY           0x18
#define ES_REG_SERIAL           0x20
#define ES_REG_DAC2_COUNT       0x28

// DAC page registers.
#define ES_PAGE_DAC             0x0C
#define ES_REG_DAC2_FRAME       0x38
#define ES_REG_DAC2_SIZE        0x3C

// Control register bits.
#define ES_CTL_BYPASS_P2        (1U << 30) // ES1373-only SRC bypass
#define ES_CTL_SYNC_RES         (1U << 14) // AC'97 warm reset
#define ES_CTL_BREQ             (1U << 7)  // PCI bus-master request
#define ES_CTL_DAC1_EN          (1U << 6)
#define ES_CTL_DAC2_EN          (1U << 5)
#define ES_CTL_ADC_EN           (1U << 4)

// Status register bits.
#define ES_STAT_AC97_RST        (1U << 29) // CT5880/ES1373 codec reset quirk

// Serial control register bits.
#define ES_P2_END_INCO(o)       (((o) & 0x07) << 19)
#define ES_P2_END_INCM          (0x07U << 19)
#define ES_P2_ST_INCO(o)        (((o) & 0x07) << 16)
#define ES_P2_ST_INCM           (0x07U << 16)
#define ES_P2_LOOP_SEL          (1U << 14)
#define ES_P2_PAUSE             (1U << 12)
#define ES_P2_INT_EN            (1U << 9)
#define ES_P2_DAC_SEN           (1U << 6)
#define ES_P2_MODEO(o)          (((o) & 0x03) << 2)
#define ES_P2_MODEM             (0x03U << 2)

// Sample rate converter register bits and RAM addresses.
#define ES_1371_SRC_RAM_ADDRM   (0x7FU << 25)
#define ES_1371_SRC_RAM_ADDRO(r) (((r) & 0x7FU) << 25)
#define ES_1371_SRC_RAM_WE      (1U << 24)
#define ES_1371_SRC_RAM_BUSY    (1U << 23)
#define ES_1371_SRC_DISABLE     (1U << 22)
#define ES_1371_DIS_P1          (1U << 21)
#define ES_1371_DIS_P2          (1U << 20)
#define ES_1371_DIS_R1          (1U << 19)

#define ES_SMPREG_DAC1          0x70
#define ES_SMPREG_DAC2          0x74
#define ES_SMPREG_ADC           0x78
#define ES_SMPREG_VOL_ADC       0x6C
#define ES_SMPREG_VOL_DAC1      0x7C
#define ES_SMPREG_VOL_DAC2      0x7E
#define ES_SMPREG_TRUNC_N       0x00
#define ES_SMPREG_INT_REGS      0x01
#define ES_SMPREG_VFREQ_FRAC    0x03

// AC'97 codec port bits and registers.
#define ES_1371_CODEC_RDY       (1U << 31)
#define ES_1371_CODEC_WIP       (1U << 30)
#define ES_1371_CODEC_READ      (1U << 23)
#define EV_1938_CODEC_MAGIC     (1U << 26)
#define AC97_RESET              0x00
#define AC97_MASTER_VOLUME      0x02
#define AC97_HEADPHONE_VOLUME   0x04
#define AC97_PCM_OUT_VOLUME     0x18
#define AC97_VENDOR_ID1         0x7C
#define AC97_VENDOR_ID2         0x7E

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
