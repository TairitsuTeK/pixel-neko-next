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
#define CMI_REG_SB_DATA        0x22
#define CMI_REG_SB_INDEX       0x23
#define CMI_REG_MIXER1         0x24
#define CMI_REG_MIXER2         0x25
#define CMI_REG_CH0_FRAME1     0x80
#define CMI_REG_CH0_FRAME2     0x84
#define CMI_REG_EXT_MISC       0x90

// FUNCTRL0 (0x00) Bits
#define CMI_CHADC0             0x00000001 // Channel 0 Direction: 0 = Playback (DAC), 1 = Capture (ADC)
#define CMI_CHADC1             0x00000002 // Channel 1 Direction: 0 = Playback, 1 = Capture
#define CMI_PAUSE0             0x00000004 // Channel 0 Pause: 1 = Paused, 0 = Running
#define CMI_PAUSE1             0x00000008 // Channel 1 Pause
#define CMI_CHEN0              0x00010000 // Channel 0 DMA Enable: 1 = Enabled, 0 = Disabled
#define CMI_CHEN1              0x00020000 // Channel 1 DMA Enable
#define CMI_RST_CH0            0x00040000 // Channel 0 Reset: 1 = Reset, 0 = Normal
#define CMI_RST_CH1            0x00080000 // Channel 1 Reset

// FUNCTRL1 (0x04) Bits
#define CMI_BREQ               0x00000010 // PCI Bus Master Request Enable

// MISC_CTRL (0x18) Bits
#define CMI_MISC_PWD           0x80000000 // Power Down (0 = normal, 1 = powered down)
#define CMI_MISC_RESET         0x40000000 // Codec Reset (1 = in reset, 0 = normal)
#define CMI_MISC_TXVX          0x08000000 // PCI Bus Master burst timing
#define CMI_MISC_N4SPK3D       0x04000000 // Copy Front to Rear (4-Speaker mode, mirrors Front/LineOut to Rear)
#define CMI_MISC_ENDBDAC       0x00800000 // Enable Double DAC
#define CMI_MISC_XCHGDAC       0x00400000 // 0: Front = CH0, 1: Front = CH1

// Extended misc magic used by ALSA cmipci for CMI8738-class chips.
#define CMI_EXT_MISC_MAGIC     0x00000920

// MIXER1 (0x24) Bits
#define CMI_MIXER1_FMMUTE      0x80       // FM Mute (1 = Mute, 0 = Unmute)
#define CMI_MIXER1_WSMUTE      0x40       // PCM Wave Mute (1 = Mute, 0 = Unmute)

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
const void* cmi8738_get_dma_buffer(void);

// Buffer write operations: half_idx: 0 = first 32KB, 1 = second 32KB
void cmi8738_write_half(int half_idx, const void* data, unsigned int bytes);
void cmi8738_silence_half(int half_idx);

// Poll DMA status: returns 0 if half 0 needs filling, 1 if half 1 needs filling, -1 if no half needed
int cmi8738_poll_half_needed(void);
void cmi8738_mark_half_filled(int half_idx);

#endif // CMI8738_H
