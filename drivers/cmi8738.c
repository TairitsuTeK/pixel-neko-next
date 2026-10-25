#include "cmi8738.h"
#include "port.h"
#include "mm.h"

static int g_cmi_present = 0;
static unsigned short g_cmi_io_base = 0;
static const char* g_cmi_chip_name = "None";
static unsigned char* g_dma_buf = 0;
static unsigned int g_dma_phys = 0;
static int g_dma_running = 0;
static int g_last_half_playing = -1;
static int g_half_filled[2] = {0, 0};

static inline unsigned int cmi_read32(unsigned int reg) {
    if (!g_cmi_io_base) return 0;
    return inl(g_cmi_io_base + reg);
}

static inline void cmi_write32(unsigned int reg, unsigned int val) {
    if (!g_cmi_io_base) return;
    outl(g_cmi_io_base + reg, val);
}

static inline unsigned short cmi_read16(unsigned int reg) {
    if (!g_cmi_io_base) return 0;
    return inw(g_cmi_io_base + reg);
}

static inline void cmi_write16(unsigned int reg, unsigned short val) {
    if (!g_cmi_io_base) return;
    outw(g_cmi_io_base + reg, val);
}

static inline void cmi_io_wait(void) {
    outb(0x80, 0);
}

static inline void cmi_write_mixer(unsigned char idx, unsigned char val) {
    if (!g_cmi_io_base) return;
    outb(g_cmi_io_base + CMI_REG_SB_INDEX, idx);
    cmi_io_wait();
    outb(g_cmi_io_base + CMI_REG_SB_DATA, val);
    cmi_io_wait();
}

static inline unsigned char cmi_read_mixer(unsigned char idx) {
    if (!g_cmi_io_base) return 0;
    outb(g_cmi_io_base + CMI_REG_SB_INDEX, idx);
    cmi_io_wait();
    return inb(g_cmi_io_base + CMI_REG_SB_DATA);
}

int cmi8738_init(void) {
    g_cmi_present = 0;
    g_cmi_io_base = 0;
    g_cmi_chip_name = "None";
    g_dma_running = 0;

    // Scan PCI bus for C-Media CMI8738 / CMI8338 across all 256 buses, 32 devices, 8 functions
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                unsigned int id = pci_read_config(bus, dev, func, 0);
                if (id == 0xFFFFFFFF || id == 0) {
                    if (func == 0) break; // Slot empty
                    continue;
                }

                unsigned short vendor = id & 0xFFFF;
                unsigned short device = (id >> 16) & 0xFFFF;

                if (vendor == CMI_VENDOR_ID) {
                    if (device == CMI_DEV_8738) {
                        g_cmi_chip_name = "C-Media CMI8738 PCI";
                    } else if (device == CMI_DEV_8338A) {
                        g_cmi_chip_name = "C-Media CMI8338A PCI";
                    } else if (device == CMI_DEV_8338B) {
                        g_cmi_chip_name = "C-Media CMI8338B PCI";
                    } else {
                        g_cmi_chip_name = "C-Media PCI Audio";
                    }

                    // Read BAR0 (I/O base)
                    unsigned int bar0 = pci_read_config(bus, dev, func, 0x10);
                    if (!(bar0 & 1)) continue; // Must be I/O space

                    g_cmi_io_base = (unsigned short)(bar0 & 0xFFFC);

                    // Enable PCI Bus Mastering (bit 2), Memory Space (bit 1), and I/O Space (bit 0)
                    unsigned int cmd = pci_read_config(bus, dev, func, 0x04);
                    pci_write_config(bus, dev, func, 0x04, cmd | 0x07);

                    g_cmi_present = 1;
                    break;
                }
            }
            if (g_cmi_present) break;
        }
        if (g_cmi_present) break;
    }

    if (!g_cmi_present || !g_cmi_io_base) {
        return -1;
    }

    // Allocate 64KB physically contiguous memory for DMA ring buffer (16 pages)
    if (!g_dma_buf) {
        g_dma_buf = (unsigned char*)pmm_alloc_pages(16);
        if (!g_dma_buf) {
            g_cmi_present = 0;
            return -2;
        }
        g_dma_phys = (unsigned int)g_dma_buf;
    }

    // Clear DMA buffer with silence (0 for 16-bit signed PCM)
    for (int i = 0; i < CMI_DMA_BUFFER_BYTES; i++) {
        g_dma_buf[i] = 0;
    }
    g_half_filled[0] = 0;
    g_half_filled[1] = 0;

    // 1. Codec Reset via MISC_CTRL (Bit 30)
    cmi_write32(CMI_REG_MISC_CTRL, CMI_MISC_RESET);
    for (volatile int d = 0; d < 2000; d++);
    cmi_write32(CMI_REG_MISC_CTRL, 0);
    for (volatile int d = 0; d < 2000; d++);

    // 2. Disable interrupts
    cmi_write32(CMI_REG_INT_HLDCLR, 0);

    // 3. Reset Channel 0 and configure FUNCTRL0:
    // Bit 0 (CMI_CHADC0) = 0 -> PLAYBACK / DAC mode (Crucial! 1 = capture/record from ADC)
    // Bit 2 (CMI_PAUSE0) = 0 -> Unpaused
    // Bit 16 (CMI_CHEN0) = 0 -> DMA disabled initially
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= CMI_RST_CH0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 2000; d++);
    f0 &= ~CMI_RST_CH0;
    f0 &= ~CMI_CHADC0; // 0 = Playback / DAC mode
    f0 &= ~CMI_PAUSE0; // 0 = Unpaused
    f0 &= ~CMI_CHEN0;  // 0 = Stopped
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    // 4. Configure MISC_CTRL:
    // - Clear CM_PWD (bit 31 = 0: normal power)
    // - Clear CM_RESET (bit 30 = 0: running)
    // - Clear CM_XCHGDAC (bit 22 = 0: Front = CH0, green jack!)
    // - Clear CM_ENDBDAC (bit 23 = 0: Single DAC playback mode)
    // - Clear bits 29:28 (Internal test monitor signals)
    // - Set CM_N4SPK3D (bit 26 = 1: Copy Front to Rear / 4-Speaker, so both green & rear jacks output)
    unsigned int misc = cmi_read32(CMI_REG_MISC_CTRL);
    misc &= ~(CMI_MISC_PWD | CMI_MISC_RESET | CMI_MISC_XCHGDAC | CMI_MISC_ENDBDAC | (3 << 28));
    misc |= CMI_MISC_N4SPK3D;
    cmi_write32(CMI_REG_MISC_CTRL, misc);

    // 5. Ensure CM_NXCHG = 0 in LEGACY_CTRL
    unsigned int leg = cmi_read32(CMI_REG_LEGACY_CTRL);
    leg &= ~0x80000000;
    cmi_write32(CMI_REG_LEGACY_CTRL, leg);

    // 6. Configure FUNCTRL1:
    // Default 44.1 kHz (DSFC = 011b, ASFC = 011b) AND Bus Master Request (CMI_BREQ, bit 4)!
    // All SPDIF bits are cleared to 0 (analog DAC mode)
    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~((7 << 10) | (7 << 13) | 0x000003C0); // clear rates & SPDIF bits
    f1 |= ((3 << 10) | (3 << 13));               // 44.1 kHz default
    f1 |= CMI_BREQ;                              // Bus Master Request
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    // 7. Format: 16-bit Stereo (CH0FMT = 11b in CHFORMAT)
    unsigned int fmt = cmi_read32(CMI_REG_CHFORMAT);
    fmt &= ~0x03;
    fmt |= 0x03;
    cmi_write32(CMI_REG_CHFORMAT, fmt);

    // 8. Native MIXER1 (0x24) and MIXER2 (0x25):
    // Writing 0x00 clears CMI_MIXER1_WSMUTE (0x40) to unmute PCM wave playback!
    outb(g_cmi_io_base + CMI_REG_MIXER1, 0x00);
    outb(g_cmi_io_base + CMI_REG_MIXER2, 0x00);

    // 9. Reset SB16 Mixer
    cmi_write_mixer(0x00, 0x00);

    // 10. Enable all output switches in SB16 MIXER_OUTMIX (0x3C)
    cmi_write_mixer(0x3C, 0x1F);

    // 11. Unmute Speaker and Phone in extended mixer register 0xF0
    unsigned char ext = cmi_read_mixer(0xF0);
    ext &= ~0x18; // Clear bit 3 (CM_VSPKM: Speaker mute) and bit 4 (CM_VPHOM: Phone mute)
    cmi_write_mixer(0xF0, ext);

    // 12. Set DMA Base Address and Frame Counts
    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFF << 16) | 0x3FFF);

    // 13. Setup Mixer: Master volume and Voice volume to 90%
    cmi8738_set_volume(90, 90);

    return 0;
}

int cmi8738_is_present(void) {
    return g_cmi_present;
}

const char* cmi8738_get_name(void) {
    return g_cmi_chip_name;
}

unsigned short cmi8738_get_io_base(void) {
    return g_cmi_io_base;
}

void cmi8738_set_format(int rate, int bits, int stereo) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    // Rate selection in FUNCTRL1 (bits 15:13 DSFC for DAC, bits 12:10 ASFC for ADC):
    // Hardware frequency divider table:
    // 0 = 5512, 1 = 11025, 2 = 22050, 3 = 44100
    // 4 = 8000, 5 = 16000, 6 = 32000, 7 = 48000
    unsigned int asfc = 3; // 44100 default
    if (rate >= 48000)      asfc = 7; // 48000 Hz
    else if (rate >= 44100) asfc = 3; // 44100 Hz
    else if (rate >= 32000) asfc = 6; // 32000 Hz
    else if (rate >= 22050) asfc = 2; // 22050 Hz
    else if (rate >= 16000) asfc = 5; // 16000 Hz
    else if (rate >= 11025) asfc = 1; // 11025 Hz
    else if (rate >= 8000)  asfc = 4; // 8000 Hz
    else                    asfc = 0; // 5512 Hz

    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~((7 << 10) | (7 << 13));
    f1 |= ((asfc << 10) | (asfc << 13));
    f1 |= CMI_BREQ; // Always preserve / assert Bus Master Request
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    // Format in CHFORMAT (bits 1:0 for Channel A / CH0)
    // 00 = 8-bit Mono, 01 = 8-bit Stereo, 10 = 16-bit Mono, 11 = 16-bit Stereo
    unsigned int chfmt = 0;
    if (bits == 16) chfmt |= 0x02;
    if (stereo)     chfmt |= 0x01;

    unsigned int fmt = cmi_read32(CMI_REG_CHFORMAT);
    fmt &= ~0x03;
    fmt |= chfmt;
    cmi_write32(CMI_REG_CHFORMAT, fmt);
}

void cmi8738_start_dma(void) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    g_last_half_playing = -1;

    // Re-program base address and buffer size
    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFF << 16) | 0x3FFF);

    // Ensure Bus Master Request (CMI_BREQ) is enabled in FUNCTRL1
    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    if (!(f1 & CMI_BREQ)) {
        cmi_write32(CMI_REG_FUNCTRL1, f1 | CMI_BREQ);
    }

    // Ensure PCM Wave output is unmuted in MIXER1 (0x24)
    unsigned char m1 = inb(g_cmi_io_base + CMI_REG_MIXER1);
    if (m1 & CMI_MIXER1_WSMUTE) {
        outb(g_cmi_io_base + CMI_REG_MIXER1, m1 & ~CMI_MIXER1_WSMUTE);
    }

    // Reset Channel 0 before starting
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= CMI_RST_CH0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 500; d++);
    f0 &= ~CMI_RST_CH0;

    // Configure Channel 0:
    // CMI_CHADC0 (bit 0) = 0 -> PLAYBACK / DAC mode!
    // CMI_PAUSE0 (bit 2) = 0 -> Unpaused
    // CMI_CHEN0  (bit 16) = 1 -> Enable DMA
    f0 &= ~(CMI_CHADC0 | CMI_PAUSE0);
    f0 |= CMI_CHEN0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    g_dma_running = 1;
}

void cmi8738_stop_dma(void) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    // Disable DMA on Channel 0 (FUNCTRL0 Bit 16)
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 &= ~CMI_CHEN0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    // Pulse reset on Channel 0
    cmi_write32(CMI_REG_FUNCTRL0, f0 | CMI_RST_CH0);
    for (volatile int d = 0; d < 500; d++);
    cmi_write32(CMI_REG_FUNCTRL0, f0 & ~CMI_RST_CH0);

    g_dma_running = 0;
    g_last_half_playing = -1;
    g_half_filled[0] = 0;
    g_half_filled[1] = 0;

    // Silence buffer
    for (int i = 0; i < CMI_DMA_BUFFER_BYTES; i++) {
        g_dma_buf[i] = 0;
    }
}

void cmi8738_pause_dma(int pause) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    if (pause) f0 |= CMI_PAUSE0;
    else       f0 &= ~CMI_PAUSE0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
}

void cmi8738_set_volume(int master_vol, int pcm_vol) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    // Unmute PCM Wave Playback in MIXER1 (Port 0x24, Bit 6)
    unsigned char m1 = inb(g_cmi_io_base + CMI_REG_MIXER1);
    m1 &= ~CMI_MIXER1_WSMUTE;
    outb(g_cmi_io_base + CMI_REG_MIXER1, m1);

    // Scale 0..100 to 5-bit (0..31) for SB16 mixer registers
    unsigned char m5 = (unsigned char)((master_vol * 31) / 100);
    unsigned char p5 = (unsigned char)((pcm_vol * 31) / 100);

    // SB16 Master Volume (0x30 Left, 0x31 Right: upper 5 bits [D7:D3])
    cmi_write_mixer(0x30, (m5 << 3));
    cmi_write_mixer(0x31, (m5 << 3));

    // SB16 Voice/PCM Volume (0x32 Left, 0x33 Right: upper 5 bits [D7:D3])
    cmi_write_mixer(0x32, (p5 << 3));
    cmi_write_mixer(0x33, (p5 << 3));

    // Also set legacy SB Pro Master (0x22) and Voice (0x04): 4 bits per channel
    unsigned char m4 = (unsigned char)((master_vol * 15) / 100);
    unsigned char p4 = (unsigned char)((pcm_vol * 15) / 100);
    cmi_write_mixer(0x22, (m4 << 4) | m4);
    cmi_write_mixer(0x04, (p4 << 4) | p4);

    // Output switch (0x3C): enable all outputs
    cmi_write_mixer(0x3C, 0x1F);

    // Speaker and Phone mute control (0xF0): clear bit 3 (CM_VSPKM) and bit 4 (CM_VPHOM)
    unsigned char ext = cmi_read_mixer(0xF0);
    ext &= ~0x18;
    cmi_write_mixer(0xF0, ext);
}

unsigned int cmi8738_get_dma_frame_pos(void) {
    if (!g_cmi_present || !g_cmi_io_base || !g_dma_running) return 0;

    // Reading Low 16 bits of CH0_FRAME2 returns remaining sample frames (16383 down to 0)
    unsigned short rem = cmi_read16(CMI_REG_CH0_FRAME2);
    if (rem > 16383) rem = 16383;
    unsigned int pos = (16384 - rem) & 16383;
    return pos;
}

const void* cmi8738_get_dma_buffer(void) {
    return g_dma_buf;
}

void cmi8738_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_dma_buf) return;
    if (half_idx < 0 || half_idx > 1) return;
    if (bytes > CMI_DMA_HALF_BYTES) bytes = CMI_DMA_HALF_BYTES;

    unsigned char* dst = g_dma_buf + (half_idx * CMI_DMA_HALF_BYTES);
    const unsigned char* src = (const unsigned char*)data;

    for (unsigned int i = 0; i < bytes; i++) {
        dst[i] = src[i];
    }
    // Zero out any remainder of the half buffer
    for (unsigned int i = bytes; i < CMI_DMA_HALF_BYTES; i++) {
        dst[i] = 0;
    }
    g_half_filled[half_idx] = 1;
}

void cmi8738_silence_half(int half_idx) {
    if (!g_dma_buf) return;
    if (half_idx < 0 || half_idx > 1) return;

    unsigned char* dst = g_dma_buf + (half_idx * CMI_DMA_HALF_BYTES);
    for (unsigned int i = 0; i < CMI_DMA_HALF_BYTES; i++) {
        dst[i] = 0;
    }
    g_half_filled[half_idx] = 1;
}

int cmi8738_poll_half_needed(void) {
    if (!g_cmi_present || !g_cmi_io_base || !g_dma_running) return -1;

    unsigned int frame = cmi8738_get_dma_frame_pos();
    // 0..8191 -> Hardware is playing Half 0. Half 1 is free to fill!
    // 8192..16383 -> Hardware is playing Half 1. Half 0 is free to fill!
    int current_playing_half = (frame < CMI_DMA_HALF_FRAMES) ? 0 : 1;

    if (current_playing_half != g_last_half_playing) {
        g_last_half_playing = current_playing_half;
        // The *other* half has finished playing and needs filling!
        int needed_half = 1 - current_playing_half;
        g_half_filled[needed_half] = 0; // Mark ready for new data
        return needed_half;
    }

    // Check if the other half was not filled yet
    int other_half = 1 - current_playing_half;
    if (!g_half_filled[other_half]) {
        return other_half;
    }

    return -1;
}

void cmi8738_mark_half_filled(int half_idx) {
    if (half_idx >= 0 && half_idx <= 1) {
        g_half_filled[half_idx] = 1;
    }
}
