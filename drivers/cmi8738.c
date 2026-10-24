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

static inline void cmi_write_mixer(unsigned char idx, unsigned char val) {
    if (!g_cmi_io_base) return;
    outb(g_cmi_io_base + CMI_REG_SB_INDEX, idx);
    outb(g_cmi_io_base + CMI_REG_SB_DATA, val);
}

static inline unsigned char cmi_read_mixer(unsigned char idx) {
    if (!g_cmi_io_base) return 0;
    outb(g_cmi_io_base + CMI_REG_SB_INDEX, idx);
    return inb(g_cmi_io_base + CMI_REG_SB_DATA);
}

int cmi8738_init(void) {
    g_cmi_present = 0;
    g_cmi_io_base = 0;
    g_cmi_chip_name = "None";
    g_dma_running = 0;

    // Scan PCI bus for C-Media CMI8738 / CMI8338
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            unsigned int id = pci_read_config(bus, slot, 0, 0);
            if (id == 0xFFFFFFFF || id == 0) continue;

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
                unsigned int bar0 = pci_read_config(bus, slot, 0, 0x10);
                if (!(bar0 & 1)) continue; // Must be I/O space

                g_cmi_io_base = (unsigned short)(bar0 & 0xFFFC);

                // Enable PCI Bus Mastering, Memory Space, and I/O Space
                unsigned int cmd = pci_read_config(bus, slot, 0, 0x04);
                pci_write_config(bus, slot, 0, 0x04, cmd | 0x05); // Bus Master (0x04) + I/O Space (0x01)

                g_cmi_present = 1;
                break;
            }
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

    // Reset Channel A (Bit 18: RST_CH0)
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= (1 << 18);
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    // Short delay and clear reset
    for (volatile int d = 0; d < 2000; d++);
    f0 &= ~(1 << 18);
    // Ensure ADC0 = 0 (Playback mode)
    f0 &= ~(1 << 2);
    // Stop DMA initially
    f0 &= ~(1 << 16);
    f0 &= ~(1 << 4); // Clear PAUSE0
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    // Default: 44.1 kHz (ASFC = 011b, bits 12:10 in FUNCTRL1)
    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~(7 << 10);
    f1 |= (3 << 10); // 011b = 44100 Hz
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    // Format: 16-bit Stereo (CH0FMT = 11b, bits 1:0 in CHFORMAT)
    unsigned int fmt = cmi_read32(CMI_REG_CHFORMAT);
    fmt &= ~0x03;
    fmt |= 0x03; // 11b = 16-bit Stereo
    cmi_write32(CMI_REG_CHFORMAT, fmt);

    // Set DMA Base Address
    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);

    // Set DMA Frame Counts:
    // Low 16 bits = 16383 (0x3FFF), High 16 bits = 8191 (0x1FFF)
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFF << 16) | 0x3FFF);

    // Setup Mixer: Master volume and Voice volume to 85%
    cmi8738_set_volume(85, 85);

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

    // Rate selection in FUNCTRL1 (bits 12:10 for Channel A)
    unsigned int asfc = 3; // 44100 default
    if (rate >= 48000) asfc = 4;      // 100b = 48000
    else if (rate >= 44100) asfc = 3; // 011b = 44100
    else if (rate >= 22050) asfc = 2; // 010b = 22050
    else if (rate >= 11025) asfc = 1; // 001b = 11025
    else asfc = 0;                    // 000b = 5512

    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~(7 << 10);
    f1 |= (asfc << 10);
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    // Format in CHFORMAT (bits 1:0 for Channel A)
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

    // Enable DMA on Channel A (FUNCTRL0 Bit 16) and unpause
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 &= ~(1 << 4); // Clear PAUSE0
    f0 |= (1 << 16); // CHEN0 = 1
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    g_dma_running = 1;
}

void cmi8738_stop_dma(void) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    // Disable DMA on Channel A (FUNCTRL0 Bit 16)
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 &= ~(1 << 16); // CHEN0 = 0
    f0 |= (1 << 4);  // PAUSE0 = 1
    cmi_write32(CMI_REG_FUNCTRL0, f0);

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
    if (pause) f0 |= (1 << 4);  // PAUSE0 = 1
    else       f0 &= ~(1 << 4); // PAUSE0 = 0
    cmi_write32(CMI_REG_FUNCTRL0, f0);
}

void cmi8738_set_volume(int master_vol, int pcm_vol) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    // Scale 0..100 to 5-bit (0..31) for CMI mixer
    unsigned char m5 = (unsigned char)((master_vol * 31) / 100);
    unsigned char p5 = (unsigned char)((pcm_vol * 31) / 100);

    // CMI8738 Native Master Volume (Reg 0x30 Left, 0x31 Right, upper 5 bits)
    cmi_write_mixer(0x30, (m5 << 3));
    cmi_write_mixer(0x31, (m5 << 3));

    // CMI8738 Native PCM Voice Volume (Reg 0x32 Left, 0x33 Right, upper 5 bits)
    cmi_write_mixer(0x32, (p5 << 3));
    cmi_write_mixer(0x33, (p5 << 3));

    // Also set legacy SB16 Mixer registers (0x22 Master, 0x04 Voice, 4 bits per channel)
    unsigned char m4 = (unsigned char)((master_vol * 15) / 100);
    unsigned char p4 = (unsigned char)((pcm_vol * 15) / 100);
    cmi_write_mixer(0x22, (m4 << 4) | m4);
    cmi_write_mixer(0x04, (p4 << 4) | p4);
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
