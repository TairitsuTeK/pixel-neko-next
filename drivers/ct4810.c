#include "ct4810.h"
#include "port.h"
#include "mm.h"

static int g_ct_present = 0;
static unsigned short g_ct_io_base = 0;
static const char* g_ct_chip_name = "None";
static unsigned char* g_ct_dma_buf = 0;
static unsigned int g_ct_dma_phys = 0;
static int g_ct_dma_running = 0;
static int g_ct_last_half_playing = -1;
static int g_ct_half_filled[2] = {0, 0};
static int g_ct_last_toggle_tick = 0;
static unsigned int g_ct_last_seen_frame = 0xFFFFFFFF;
static int g_ct_hw_reg_moving = 0;

static inline unsigned int ct_read32(unsigned int reg) {
    if (!g_ct_io_base) return 0;
    return inl(g_ct_io_base + reg);
}

static inline void ct_write32(unsigned int reg, unsigned int val) {
    if (!g_ct_io_base) return;
    outl(g_ct_io_base + reg, val);
}

static inline unsigned short ct_read16(unsigned int reg) {
    if (!g_ct_io_base) return 0;
    return inw(g_ct_io_base + reg);
}

static inline void ct_write16(unsigned int reg, unsigned short val) {
    if (!g_ct_io_base) return;
    outw(g_ct_io_base + reg, val);
}

// Wait for Sample Rate Converter (SRC) to report not busy
static unsigned int ct_wait_src_ready(void) {
    if (!g_ct_io_base) return 0;
    for (int t = 0; t < 20000; t++) {
        unsigned int r = ct_read32(ES_REG_SRC);
        if (!(r & ES_1371_SRC_RAM_BUSY)) {
            return r;
        }
        for (volatile int d = 0; d < 20; d++);
    }
    return 0;
}

// Write to SRC RAM register
static void ct_src_write(unsigned short reg, unsigned short data) {
    if (!g_ct_io_base) return;
    unsigned int r = ct_wait_src_ready() & (ES_1371_SRC_DISABLE | ES_1371_DIS_P1 | ES_1371_DIS_P2 | ES_1371_DIS_R1);
    r |= ((unsigned int)(reg & 0x7F) << 25) | (data & 0xFFFF);
    ct_write32(ES_REG_SRC, r | ES_1371_SRC_RAM_WE);
}

// Read from SRC RAM register
static unsigned short ct_src_read(unsigned short reg) {
    if (!g_ct_io_base) return 0;
    unsigned int r = ct_wait_src_ready() & (ES_1371_SRC_DISABLE | ES_1371_DIS_P1 | ES_1371_DIS_P2 | ES_1371_DIS_R1);
    r |= ((unsigned int)(reg & 0x7F) << 25);
    ct_write32(ES_REG_SRC, r);
    ct_wait_src_ready();
    return (unsigned short)(ct_read32(ES_REG_SRC) & 0xFFFF);
}

// Write to AC'97 Codec register with required SRC synchronization
static void ct_codec_write(unsigned char reg, unsigned short val) {
    if (!g_ct_io_base) return;

    for (int t = 0; t < 20000; t++) {
        if (!(ct_read32(ES_REG_CODEC) & ES_1371_CODEC_WIP)) {
            // 1. Save current SRC state
            unsigned int x = ct_wait_src_ready();
            ct_write32(ES_REG_SRC, (x & (ES_1371_SRC_DISABLE | ES_1371_DIS_P1 |
                                         ES_1371_DIS_P2 | ES_1371_DIS_R1)) | 0x00010000);

            // 2. Wait for not busy (state 0) to avoid transition glitch
            for (int t2 = 0; t2 < 20000; t2++) {
                if ((ct_read32(ES_REG_SRC) & 0x00870000) == 0x00000000)
                    break;
                for (volatile int d = 0; d < 10; d++);
            }

            // 3. Wait for safe synchronization state (0x00010000)
            for (int t2 = 0; t2 < 20000; t2++) {
                if ((ct_read32(ES_REG_SRC) & 0x00870000) == 0x00010000)
                    break;
                for (volatile int d = 0; d < 10; d++);
            }

            // 4. Output AC'97 Codec register write: reg in bits 22:16, data in bits 15:0
            ct_write32(ES_REG_CODEC, ((unsigned int)(reg & 0x7F) << 16) | (val & 0xFFFF));

            // 5. Restore SRC register state
            ct_wait_src_ready();
            ct_write32(ES_REG_SRC, x);
            return;
        }
        for (volatile int d = 0; d < 20; d++);
    }
}

// Read from AC'97 Codec register
static unsigned short ct_codec_read(unsigned char reg) {
    if (!g_ct_io_base) return 0;

    for (int t = 0; t < 20000; t++) {
        if (!(ct_read32(ES_REG_CODEC) & ES_1371_CODEC_WIP)) {
            ct_write32(ES_REG_CODEC, (1 << 23) | ((unsigned int)(reg & 0x7F) << 16));
            break;
        }
        for (volatile int d = 0; d < 20; d++);
    }
    for (int t = 0; t < 20000; t++) {
        if (!(ct_read32(ES_REG_CODEC) & ES_1371_CODEC_WIP)) {
            break;
        }
        for (volatile int d = 0; d < 20; d++);
    }
    for (int t = 0; t < 20000; t++) {
        unsigned int x = ct_read32(ES_REG_CODEC);
        if (!(x & ES_1371_CODEC_PORD)) {
            return (unsigned short)(x & 0xFFFF);
        }
        for (volatile int d = 0; d < 20; d++);
    }
    return 0;
}

int ct4810_init(void) {
    g_ct_present = 0;
    g_ct_io_base = 0;
    g_ct_chip_name = "None";
    g_ct_dma_running = 0;

    // Scan PCI bus for Ensoniq ES1371 / ES1373 or Creative CT5880 / CT4810 / EV1938
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                unsigned int id = pci_read_config(bus, dev, func, 0);
                if (id == 0xFFFFFFFF || id == 0) {
                    if (func == 0) break;
                    continue;
                }

                unsigned short vendor = id & 0xFFFF;
                unsigned short device = (id >> 16) & 0xFFFF;

                if ((vendor == ENSONIQ_VENDOR_ID && (device == ES1371_DEV_ID || device == CT5880_DEV_ID)) ||
                    (vendor == CREATIVE_VENDOR_ID && (device == CT5880_DEV_ID || device == EV1938_DEV_ID))) {

                    if (device == CT5880_DEV_ID) {
                        g_ct_chip_name = "Creative CT4810 (CT5880) PCI";
                    } else if (device == EV1938_DEV_ID) {
                        g_ct_chip_name = "Creative AudioPCI (EV1938) PCI";
                    } else {
                        g_ct_chip_name = "Ensoniq AudioPCI (ES1371) PCI";
                    }

                    // Read BAR0 (I/O base)
                    unsigned int bar0 = pci_read_config(bus, dev, func, 0x10);
                    if (!(bar0 & 1)) continue;

                    g_ct_io_base = (unsigned short)(bar0 & 0xFFFC);

                    // Enable Bus Master (bit 2), Memory (bit 1), and I/O (bit 0)
                    unsigned int cmd = pci_read_config(bus, dev, func, 0x04);
                    pci_write_config(bus, dev, func, 0x04, cmd | 0x07);

                    // Configure PCI Latency Timer (0x0C)
                    unsigned int lat = pci_read_config(bus, dev, func, 0x0C);
                    if ((lat & 0xFF00) < 0x2000) {
                        pci_write_config(bus, dev, func, 0x0C, (lat & 0xFFFF00FF) | 0x4000);
                    }

                    g_ct_present = 1;
                    break;
                }
            }
            if (g_ct_present) break;
        }
        if (g_ct_present) break;
    }

    if (!g_ct_present || !g_ct_io_base) {
        return -1;
    }

    // Allocate 64KB physically contiguous memory for DMA buffer
    if (!g_ct_dma_buf) {
        g_ct_dma_buf = (unsigned char*)pmm_alloc_pages_aligned(16, 16);
        if (!g_ct_dma_buf) {
            g_ct_present = 0;
            return -2;
        }
        g_ct_dma_phys = (unsigned int)g_ct_dma_buf;
    }

    // Clear buffer with silence
    for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) {
        g_ct_dma_buf[i] = 0;
    }
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    // Reset Control Register: stop all DACs
    ct_write32(ES_REG_CONTROL, 0x00000000);
    ct_write32(ES_REG_SERIAL, 0x00000000);
    for (volatile int d = 0; d < 2000; d++);

    // Initialize SRC: clear all 128 SRC RAM registers
    for (int i = 0; i < 0x80; i++) {
        ct_src_write(i, 0);
    }
    // Set default truncation / interpolation parameters for DAC1, DAC2, ADC:
    ct_src_write(0x70 /* DAC1 */, 16 << 4);
    ct_src_write(0x74 /* DAC2 */, 16 << 4);
    ct_src_write(0x78 /* ADC  */, 16 << 4);

    // Reset AC'97 Codec
    ct_codec_write(0x00, 0x0000); // Cold reset AC'97 Codec
    for (volatile int d = 0; d < 5000; d++);

    // Unmute Codec volumes (Master, Headphone, PCM Out) to 0dB attenuation
    ct_codec_write(0x02, 0x0000); // Master Volume L/R = 0dB
    ct_codec_write(0x04, 0x0000); // Headphone Volume L/R = 0dB
    ct_codec_write(0x18, 0x0000); // PCM Out Volume L/R = 0dB
    ct_codec_write(0x0A, 0x8000); // Mute PC Beep
    ct_codec_write(0x0E, 0x8008); // Mute Mic
    ct_codec_write(0x10, 0x8808); // Mute Line In
    ct_codec_write(0x12, 0x8808); // Mute CD In

    // Program DAC2 default format (44.1 kHz, 16-bit stereo)
    ct4810_set_format(44100, 16, 1);

    // Program MEMPAGE to PAGE 0x0C (ES_PAGE_DAC)
    ct_write32(ES_REG_MEMPAGE, ES_PAGE_DAC);
    ct_write32(ES_REG_DAC2_FRAME1, g_ct_dma_phys);
    ct_write32(ES_REG_DAC2_FRAME2, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(ES_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);

    return 0;
}

int ct4810_is_present(void) {
    return g_ct_present;
}

const char* ct4810_get_name(void) {
    return g_ct_chip_name;
}

unsigned short ct4810_get_io_base(void) {
    return g_ct_io_base;
}

void ct4810_set_format(int rate, int bits, int stereo) {
    if (!g_ct_present || !g_ct_io_base) return;

    // 1. Configure Serial Control (0x20): Mode (16-bit Stereo = 3)
    unsigned int mode = 0;
    if (bits == 16) mode |= 0x02;
    if (stereo)     mode |= 0x01;

    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    sctrl &= ~(ES_P2_LOOP_SEL | ES_P2_PAUSE | ES_P2_MODEM);
    sctrl |= ES_P2_MODEO(mode); // Loop mode (bit 14=0), Not paused (bit 12=0)
    ct_write32(ES_REG_SERIAL, sctrl);

    // 2. Configure SRC for DAC2
    // freq = ((rate << 15) + 1500) / 3000
    // rate * 32768 fits in 32-bit: 48000 * 32768 = 1,572,864,000 < 4,294,967,295
    unsigned int freq = (((unsigned int)rate << 15) + 1500U) / 3000U;

    // Freeze DAC2 accumulator while updating rate
    unsigned int r = (ct_wait_src_ready() & (ES_1371_SRC_DISABLE | ES_1371_DIS_P1 | ES_1371_DIS_R1)) | ES_1371_DIS_P2;
    ct_write32(ES_REG_SRC, r);

    // Update INT_REGS (0x75) and VFREQ_FRAC (0x77)
    unsigned short cur_int = ct_src_read(0x74 + 1);
    ct_src_write(0x74 + 1, (cur_int & 0x00FF) | ((freq >> 5) & 0xFC00));
    ct_src_write(0x74 + 3, freq & 0x7FFF);

    // Unfreeze DAC2 accumulator
    r = ct_wait_src_ready() & (ES_1371_SRC_DISABLE | ES_1371_DIS_P1 | ES_1371_DIS_R1);
    ct_write32(ES_REG_SRC, r);
}

void ct4810_start_dma(void) {
    if (!g_ct_present || !g_ct_io_base) return;

    extern volatile int global_tick;

    // 1. Program Page to DAC (0x0C) and Buffer Parameters
    ct_write32(ES_REG_MEMPAGE, ES_PAGE_DAC);
    ct_write32(ES_REG_DAC2_FRAME1, g_ct_dma_phys);
    ct_write32(ES_REG_DAC2_FRAME2, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(ES_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);

    // 2. Ensure Serial Control is loop mode, not paused
    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    sctrl &= ~(ES_P2_LOOP_SEL | ES_P2_PAUSE);
    ct_write32(ES_REG_SERIAL, sctrl);

    // 3. Ensure AC'97 volumes are unmuted
    ct_codec_write(0x02, 0x0000);
    ct_codec_write(0x18, 0x0000);

    // 4. Enable DAC2 in Control Register (0x00)
    unsigned int ctl = ct_read32(ES_REG_CONTROL);
    ctl |= ES_CTL_DAC2_EN;
    ct_write32(ES_REG_CONTROL, ctl);

    g_ct_dma_running = 1;
    g_ct_last_half_playing = 0;
    g_ct_last_toggle_tick = global_tick;
    g_ct_last_seen_frame = 0xFFFFFFFF;
    g_ct_hw_reg_moving = 0;
}

void ct4810_stop_dma(void) {
    if (!g_ct_present || !g_ct_io_base) return;

    // Disable DAC2 in Control Register
    unsigned int ctl = ct_read32(ES_REG_CONTROL);
    ctl &= ~ES_CTL_DAC2_EN;
    ct_write32(ES_REG_CONTROL, ctl);

    // Pause in Serial Control
    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    sctrl |= ES_P2_PAUSE;
    ct_write32(ES_REG_SERIAL, sctrl);

    g_ct_dma_running = 0;
    g_ct_last_half_playing = -1;
    g_ct_last_toggle_tick = 0;
    g_ct_last_seen_frame = 0xFFFFFFFF;
    g_ct_hw_reg_moving = 0;
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) {
        g_ct_dma_buf[i] = 0;
    }
}

void ct4810_pause_dma(int pause) {
    if (!g_ct_present || !g_ct_io_base) return;

    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    if (pause) sctrl |= ES_P2_PAUSE;
    else       sctrl &= ~ES_P2_PAUSE;
    ct_write32(ES_REG_SERIAL, sctrl);
}

void ct4810_set_volume(int master_vol, int pcm_vol) {
    if (!g_ct_present || !g_ct_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    // AC'97 volume attenuation: 0 = 0dB (loudest), 31 = -46.5dB (quietest), bit 15 = mute
    unsigned char m_att = (unsigned char)((100 - master_vol) * 31 / 100);
    unsigned char p_att = (unsigned char)((100 - pcm_vol) * 31 / 100);

    unsigned short m_val = (master_vol == 0) ? 0x8000 : ((unsigned short)m_att << 8) | m_att;
    unsigned short p_val = (pcm_vol == 0) ? 0x8000 : ((unsigned short)p_att << 8) | p_att;

    ct_codec_write(0x02, m_val); // Master Volume
    ct_codec_write(0x04, m_val); // Headphone Volume
    ct_codec_write(0x18, p_val); // PCM Out Volume
}

unsigned int ct4810_get_dma_frame_pos(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return 0;

    // Read 32-bit register at 0x3C in PAGE 0x0C (ES_PAGE_DAC)
    // Upper 16 bits (val >> 16) contains the current sample count (0..16383)
    ct_write32(ES_REG_MEMPAGE, ES_PAGE_DAC);
    unsigned int val = ct_read32(ES_REG_DAC2_FRAME2);
    unsigned int frame = (val >> 16) & 0xFFFF;
    if (frame >= CT_DMA_TOTAL_FRAMES) frame = 0;

    return frame;
}

const void* ct4810_get_dma_buffer(void) {
    return g_ct_dma_buf;
}

void ct4810_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_ct_dma_buf) return;
    if (half_idx < 0 || half_idx > 1) return;
    if (bytes > CT_DMA_HALF_BYTES) bytes = CT_DMA_HALF_BYTES;

    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    const unsigned char* src = (const unsigned char*)data;
    for (unsigned int i = 0; i < bytes; i++) {
        dest[i] = src[i];
    }
    for (unsigned int i = bytes; i < CT_DMA_HALF_BYTES; i++) {
        dest[i] = 0;
    }

    g_ct_half_filled[half_idx] = 1;
}

void ct4810_silence_half(int half_idx) {
    if (!g_ct_dma_buf) return;
    if (half_idx < 0 || half_idx > 1) return;

    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    for (unsigned int i = 0; i < CT_DMA_HALF_BYTES; i++) {
        dest[i] = 0;
    }
    g_ct_half_filled[half_idx] = 0;
}

int ct4810_poll_half_needed(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return -1;

    extern volatile int global_tick;

    unsigned int frame = ct4810_get_dma_frame_pos();

    // Check if hardware frame counter has actually advanced from initial read
    if (g_ct_last_seen_frame == 0xFFFFFFFF) {
        g_ct_last_seen_frame = frame;
    } else if (frame != g_ct_last_seen_frame) {
        g_ct_last_seen_frame = frame;
        g_ct_hw_reg_moving = 1;
    }

    int current_playing_half = (frame < CT_DMA_HALF_FRAMES) ? 0 : 1;

    if (g_ct_last_half_playing < 0) {
        g_ct_last_half_playing = current_playing_half;
        g_ct_last_toggle_tick = global_tick;
    }

    if (g_ct_hw_reg_moving) {
        if (current_playing_half != g_ct_last_half_playing) {
            g_ct_last_half_playing = current_playing_half;
            g_ct_last_toggle_tick = global_tick;
            int needed_half = 1 - current_playing_half;
            g_ct_half_filled[needed_half] = 0;
            return needed_half;
        }

        int other_half = 1 - current_playing_half;
        if (!g_ct_half_filled[other_half]) {
            return other_half;
        }
        return -1;
    }

    // Software timeout watchdog fallback: 19 ticks = ~185ms
    if (g_ct_last_toggle_tick == 0) g_ct_last_toggle_tick = global_tick;
    if ((global_tick - g_ct_last_toggle_tick) >= 19) {
        g_ct_last_toggle_tick = global_tick;
        int needed = g_ct_last_half_playing;
        g_ct_last_half_playing = 1 - g_ct_last_half_playing;
        g_ct_half_filled[needed] = 0;
        return needed;
    }

    if (!g_ct_half_filled[0]) return 0;
    if (!g_ct_half_filled[1]) return 1;

    return -1;
}

void ct4810_mark_half_filled(int half_idx) {
    if (half_idx >= 0 && half_idx < 2) {
        g_ct_half_filled[half_idx] = 1;
    }
}
