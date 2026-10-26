#include "cmi8738.h"
#include "port.h"
#include "mm.h"
#include "dma.h"
#include "pci.h"

static int g_cmi_present = 0;
static unsigned short g_cmi_io_base = 0;
static unsigned short g_cmi_device = 0;
static const char* g_cmi_chip_name = "None";
static unsigned char* g_dma_buf = 0;
static unsigned int g_dma_phys = 0;
static dma_buffer_t g_dma;
static int g_dma_running = 0;
static int g_last_half_playing = -1;
static int g_half_filled[2] = {0, 0};
static int g_last_toggle_tick = 0;
static unsigned int g_last_seen_frame = 0xFFFFFFFF;
static int g_hw_reg_moving = 0;

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
    g_cmi_device = 0;
    g_cmi_chip_name = "None";
    g_dma_running = 0;

    pci_scan_all();
    for (int index = 0; index < pci_get_device_count(); index++) {
        const pci_device_t* pci = pci_get_device(index);
        if (!pci || pci->vendor != CMI_VENDOR_ID) continue;
        unsigned short device = pci->device;

        if (device == CMI_DEV_8738) {
            g_cmi_chip_name = "C-Media CMI8738 PCI";
        } else if (device == CMI_DEV_8338A) {
            g_cmi_chip_name = "C-Media CMI8338A PCI";
        } else if (device == CMI_DEV_8338B) {
            g_cmi_chip_name = "C-Media CMI8338B PCI";
        } else {
            g_cmi_chip_name = "C-Media PCI Audio";
        }

        if (!pci->bar_valid[0] || !pci->bar_is_io[0]) continue;

        g_cmi_io_base = (unsigned short)pci->bar[0];
        g_cmi_device = device;

        pci_enable_io(pci);
        pci_enable_memory(pci);
        pci_enable_bus_master(pci);

        // Configure PCI latency to at least 64 clocks for reliable DMA bursts.
        unsigned int lat = pci_config_read(pci->bus, pci->slot,
                                           pci->function, 0x0C);
        if ((lat & 0xFF00) < 0x2000) {
            pci_config_write(pci->bus, pci->slot, pci->function,
                             0x0C, (lat & 0xFFFF00FF) | 0x4000);
        }

        g_cmi_present = 1;
        break;
    }

    if (!g_cmi_present || !g_cmi_io_base) {
        return -1;
    }

    // Allocate 64KB physically contiguous memory for DMA ring buffer (16 pages, 64KB aligned)
    if (!dma_is_valid(&g_dma)) {
        g_dma = dma_alloc_aligned(CMI_DMA_BUFFER_BYTES, 65536);
        if (!dma_is_valid(&g_dma)) {
            g_cmi_present = 0;
            return -2;
        }
        g_dma_buf = (unsigned char*)g_dma.virt;
        g_dma_phys = g_dma.phys;
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
    // Bit 1 (CMI_CHADC1) = 1 -> CAPTURE / ADC mode for Channel 1 (ALSA default)
    // Bit 2 (CMI_PAUSE0) = 0 -> Unpaused
    // Bit 16 (CMI_CHEN0) = 0 -> DMA disabled initially
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= CMI_RST_CH0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 2000; d++);
    f0 &= ~CMI_RST_CH0;
    f0 &= ~CMI_CHADC0; // 0 = Playback / DAC mode
    f0 |= CMI_CHADC1;  // 1 = Capture mode for Channel 1
    f0 &= ~CMI_PAUSE0; // 0 = Unpaused
    f0 &= ~CMI_CHEN0;  // 0 = Stopped
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    // 4. Configure MISC_CTRL:
    // - Clear CM_PWD (bit 31 = 0: normal power)
    // - Clear CM_RESET (bit 30 = 0: running)
    // - Set CM_ENDBDAC (bit 23 = 1: dual-DAC routing required by ALSA)
    // - Clear CM_XCHGDAC (bit 22 = 0: front output = CH0, green jack)
    // - Set CM_TXVX (bit 27 = 1: PCI bus master burst timing)
    // - Set CM_N4SPK3D (bit 26 = 1: Copy Front to Rear / 4-Speaker, so both green & rear jacks output)
    unsigned int misc = cmi_read32(CMI_REG_MISC_CTRL);
    misc &= ~(CMI_MISC_PWD | CMI_MISC_RESET | CMI_MISC_XCHGDAC);
    misc |= (CMI_MISC_ENDBDAC | CMI_MISC_N4SPK3D);
    if (g_cmi_device == CMI_DEV_8738) misc |= CMI_MISC_TXVX;
    cmi_write32(CMI_REG_MISC_CTRL, misc);

    // 5. CMI8738-class parts require the ALSA cmipci extended-routing magic.
    if (g_cmi_device == CMI_DEV_8338A || g_cmi_device == CMI_DEV_8338B) {
        cmi_write32(CMI_REG_EXT_MISC, 0);
    } else {
        cmi_write32(CMI_REG_EXT_MISC, CMI_EXT_MISC_MAGIC);
    }

    // 6. Ensure CM_NXCHG = 0 in LEGACY_CTRL
    unsigned int leg = cmi_read32(CMI_REG_LEGACY_CTRL);
    leg &= ~0x80000000;
    cmi_write32(CMI_REG_LEGACY_CTRL, leg);

    // 7. Configure FUNCTRL1:
    // Default 44.1 kHz (DSFC = 011b, ASFC = 011b) AND Bus Master Request (CMI_BREQ, bit 4)!
    // All SPDIF bits are cleared to 0 (analog DAC mode)
    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~((7 << 10) | (7 << 13) | 0x000003C0); // clear rates & SPDIF bits
    f1 |= ((3 << 10) | (3 << 13));               // 44.1 kHz default
    f1 |= CMI_BREQ;                              // Bus Master Request
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    // 8. Format: 16-bit Stereo (CH0FMT = 11b in CHFORMAT) and CM_SETLAT48 (0x00800000)
    unsigned int fmt = cmi_read32(CMI_REG_CHFORMAT);
    fmt &= ~0x03;
    fmt |= 0x03;
    fmt |= 0x00800000; // CM_SETLAT48
    cmi_write32(CMI_REG_CHFORMAT, fmt);

    // 9. Native MIXER1 (0x24) and MIXER2 (0x25):
    // Writing 0x00 clears CMI_MIXER1_WSMUTE (0x40) to unmute PCM wave playback!
    outb(g_cmi_io_base + CMI_REG_MIXER1, 0x00);
    outb(g_cmi_io_base + CMI_REG_MIXER2, 0x00);

    // 10. Reset SB16 Mixer
    cmi_write_mixer(0x00, 0x00);

    // 11. Enable all output switches in SB16 MIXER_OUTMIX (0x3C)
    cmi_write_mixer(0x3C, 0x1F);

    // 12. Unmute Speaker and Phone in extended mixer register 0xF0
    unsigned char ext = cmi_read_mixer(0xF0);
    ext &= ~0x18; // Clear bit 3 (CM_VSPKM: Speaker mute) and bit 4 (CM_VPHOM: Phone mute)
    cmi_write_mixer(0xF0, ext);

    // 13. Set DMA Base Address and Frame Counts
    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFF << 16) | 0x3FFF);

    // 14. Setup Mixer: Master volume and Voice volume to 90%
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
    fmt |= 0x00800000; // Preserve CM_SETLAT48
    cmi_write32(CMI_REG_CHFORMAT, fmt);
}

void cmi8738_start_dma(void) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    extern volatile int global_tick;

    // 1. Reset Channel 0 FIRST before setting frame/buffer registers
    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= CMI_RST_CH0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 500; d++);
    f0 &= ~CMI_RST_CH0;
    f0 &= ~(CMI_CHADC0 | CMI_PAUSE0);
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 500; d++);

    // 2. Program DMA Base Address (CH0_FRAME1) and Buffer Sizes (CH0_FRAME2)
    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFF << 16) | 0x3FFF);

    // 3. Ensure Bus Master Request (CMI_BREQ) is enabled in FUNCTRL1
    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    if (!(f1 & CMI_BREQ)) {
        cmi_write32(CMI_REG_FUNCTRL1, f1 | CMI_BREQ);
    }

    // 4. Ensure PCM Wave output is unmuted in MIXER1 (0x24)
    unsigned char m1 = inb(g_cmi_io_base + CMI_REG_MIXER1);
    if (m1 & CMI_MIXER1_WSMUTE) {
        outb(g_cmi_io_base + CMI_REG_MIXER1, m1 & ~CMI_MIXER1_WSMUTE);
    }

    // 5. Enable DMA on Channel 0 (CHEN0)
    f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 &= ~(CMI_CHADC0 | CMI_PAUSE0);
    f0 |= CMI_CHEN0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    g_dma_running = 1;
    g_last_half_playing = 0; // Hardware begins playing from Half 0
    g_last_toggle_tick = global_tick;
    g_last_seen_frame = 0xFFFFFFFF;
    g_hw_reg_moving = 0;
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
    g_last_toggle_tick = 0;
    g_last_seen_frame = 0xFFFFFFFF;
    g_hw_reg_moving = 0;
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

    // ALSA cmipci uses 5-bit stereo SB mixer volumes at 0x22/0x23 (master)
    // and 0x04/0x05 (PCM). Other apparent "output" registers are not mutes.
    unsigned char m5 = (unsigned char)((master_vol * 31) / 100);
    unsigned char p5 = (unsigned char)((pcm_vol * 31) / 100);

    cmi_write_mixer(0x22, (m5 << 3));
    cmi_write_mixer(0x23, (m5 << 3));
    cmi_write_mixer(0x04, (p5 << 3));
    cmi_write_mixer(0x05, (p5 << 3));

    // Output switch (0x3C): enable all analog mixer outputs.
    cmi_write_mixer(0x3C, 0x1F);

    // Speaker and Phone mute control (0xF0): clear bit 3 (CM_VSPKM) and bit 4 (CM_VPHOM)
    unsigned char ext = cmi_read_mixer(0xF0);
    ext &= ~0x18;
    cmi_write_mixer(0xF0, ext);
}

unsigned int cmi8738_get_dma_frame_pos(void) {
    if (!g_cmi_present || !g_cmi_io_base || !g_dma_running) return 0;

    // Reading Low 16 bits of CH0_FRAME2 returns remaining sample frames (16383 down to 0)
    // Retry up to 3 times to get a stable, valid reading (ALSA cmipci workaround)
    unsigned short rem = 0;
    for (int tries = 0; tries < 3; tries++) {
        rem = cmi_read16(CMI_REG_CH0_FRAME2);
        if (rem < CMI_DMA_TOTAL_FRAMES) break;
    }
    if (rem >= CMI_DMA_TOTAL_FRAMES) rem = CMI_DMA_TOTAL_FRAMES - 1;
    unsigned int pos = (CMI_DMA_TOTAL_FRAMES - (rem + 1)) & (CMI_DMA_TOTAL_FRAMES - 1);
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
    dma_flush(&g_dma);
    g_half_filled[half_idx] = 1;
}

void cmi8738_silence_half(int half_idx) {
    if (!g_dma_buf) return;
    if (half_idx < 0 || half_idx > 1) return;

    unsigned char* dst = g_dma_buf + (half_idx * CMI_DMA_HALF_BYTES);
    for (unsigned int i = 0; i < CMI_DMA_HALF_BYTES; i++) {
        dst[i] = 0;
    }
    dma_flush(&g_dma);
    g_half_filled[half_idx] = 1;
}

int cmi8738_poll_half_needed(void) {
    if (!g_cmi_present || !g_cmi_io_base || !g_dma_running) return -1;

    extern volatile int global_tick;

    unsigned int frame = cmi8738_get_dma_frame_pos();

    // Check if the hardware frame register has actually advanced from initial read
    if (g_last_seen_frame == 0xFFFFFFFF) {
        g_last_seen_frame = frame;
    } else if (frame != g_last_seen_frame) {
        g_last_seen_frame = frame;
        g_hw_reg_moving = 1;
    }

    // 0..8191 -> Hardware is playing Half 0. Half 1 is free to fill!
    // 8192..16383 -> Hardware is playing Half 1. Half 0 is free to fill!
    int current_playing_half = (frame < CMI_DMA_HALF_FRAMES) ? 0 : 1;

    if (g_last_half_playing < 0) {
        g_last_half_playing = current_playing_half;
        g_last_toggle_tick = global_tick;
    }

    if (g_hw_reg_moving) {
        // Hardware register is functioning properly!
        if (current_playing_half != g_last_half_playing) {
            g_last_half_playing = current_playing_half;
            g_last_toggle_tick = global_tick;
            // The *other* half has finished playing and needs filling!
            int needed_half = 1 - current_playing_half;
            g_half_filled[needed_half] = 0;
            return needed_half;
        }

        // Check if the other half was not filled yet
        int other_half = 1 - current_playing_half;
        if (!g_half_filled[other_half]) {
            return other_half;
        }

        return -1;
    }

    // Fallback ONLY when hardware register is completely stuck:
    // Pace strictly by system timer (8192 frames at 44.1k is ~185ms, 18-19 ticks at 100Hz)
    if (g_last_toggle_tick == 0) g_last_toggle_tick = global_tick;
    if ((global_tick - g_last_toggle_tick) >= 19) {
        g_last_toggle_tick = global_tick;
        int needed = g_last_half_playing; // The half that just completed
        g_last_half_playing = 1 - g_last_half_playing;
        g_half_filled[needed] = 0;
        return needed;
    }

    // Check if either half has never been filled
    if (!g_half_filled[0]) return 0;
    if (!g_half_filled[1]) return 1;

    return -1;
}

void cmi8738_mark_half_filled(int half_idx) {
    if (half_idx >= 0 && half_idx <= 1) {
        g_half_filled[half_idx] = 1;
    }
}
