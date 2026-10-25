#include "ct4810.h"
#include "port.h"
#include "mm.h"
#include "timer.h"

#define CT_SRC_STATE_MASK \
    (ES_1371_SRC_DISABLE | ES_1371_DIS_P1 | ES_1371_DIS_P2 | ES_1371_DIS_R1)
#define CT_CODEC_SYNC_STATE 0x00010000U
#define CT_CODEC_STATE_MASK 0x00870000U
#define CT_WAIT_LOOPS       0x8000

static int g_ct_present = 0;
static unsigned short g_ct_io_base = 0;
static const char* g_ct_chip_name = "None";
static char g_ct_display_name[72];
static unsigned char* g_ct_dma_buf = 0;
static unsigned int g_ct_dma_phys = 0;
static int g_ct_dma_running = 0;
static int g_ct_last_half_playing = -1;
static int g_ct_half_filled[2] = {0, 0};
static int g_ct_last_toggle_tick = 0;
static unsigned int g_ct_last_seen_frame = 0xFFFFFFFF;
static int g_ct_hw_reg_moving = 0;
static unsigned char g_ct_revision = 0;
static int g_ct_is_ev1938 = 0;

static inline unsigned int ct_read32(unsigned int reg) {
    if (!g_ct_io_base) return 0;
    return inl(g_ct_io_base + reg);
}

static inline void ct_write32(unsigned int reg, unsigned int val) {
    if (!g_ct_io_base) return;
    outl(g_ct_io_base + reg, val);
}

static inline void ct_io_delay(void) {
    outb(0x80, 0);
}

static void ct_copy_string(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void ct_append_string(char* dst, const char* suffix, int max_len) {
    int pos = 0;
    while (pos < max_len - 1 && dst[pos]) pos++;
    for (int i = 0; suffix[i] && pos < max_len - 1; i++) {
        dst[pos++] = suffix[i];
    }
    dst[pos] = '\0';
}

static void ct_set_codec_name(unsigned short id1, unsigned short id2) {
    ct_copy_string(g_ct_display_name, g_ct_chip_name, sizeof(g_ct_display_name));

    if (id1 == 0x414B && id2 == 0x4D00) {
        ct_append_string(g_ct_display_name, " / AK4540 AC'97", sizeof(g_ct_display_name));
    } else if (id1 == 0x5452 && id2 == 0x4103) {
        ct_append_string(g_ct_display_name, " / TR28023 (CT1297) AC'97",
                         sizeof(g_ct_display_name));
    }

    if (g_ct_display_name[0]) {
        g_ct_chip_name = g_ct_display_name;
    }
}

// Wait until the ES1371 sample-rate converter RAM accepts a command.
static unsigned int ct_wait_src_ready(void) {
    unsigned int r = 0;
    if (!g_ct_io_base) return 0;
    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        r = ct_read32(ES_REG_SRC);
        if (!(r & ES_1371_SRC_RAM_BUSY)) return r;
        ct_io_delay();
    }
    return 0;
}

// ALSA exposes the SRC state before reading its RAM. The chip needs the
// intermediate 0x10000 state and a second access to return valid data.
static unsigned short ct_src_read(unsigned short reg) {
    unsigned int temp;
    unsigned int orig;
    unsigned int r;

    if (!g_ct_io_base) return 0;

    temp = orig = ct_wait_src_ready();
    r = (temp & CT_SRC_STATE_MASK) | ES_1371_SRC_RAM_ADDRO(reg) |
        CT_CODEC_SYNC_STATE;
    ct_write32(ES_REG_SRC, r);

    temp = ct_wait_src_ready();
    if ((temp & CT_CODEC_STATE_MASK) != CT_CODEC_SYNC_STATE) {
        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            temp = ct_read32(ES_REG_SRC);
            if ((temp & CT_CODEC_STATE_MASK) == CT_CODEC_SYNC_STATE) break;
            ct_io_delay();
        }
    }

    r = (orig & CT_SRC_STATE_MASK) | ES_1371_SRC_RAM_ADDRO(reg);
    ct_write32(ES_REG_SRC, r);
    return (unsigned short)(temp & 0xFFFF);
}

static void ct_src_write(unsigned short reg, unsigned short data) {
    unsigned int r = ct_wait_src_ready() & CT_SRC_STATE_MASK;
    r |= ES_1371_SRC_RAM_ADDRO(reg) | (unsigned int)data;
    ct_write32(ES_REG_SRC, r | ES_1371_SRC_RAM_WE);
}

static unsigned int ct_enter_codec_window(void) {
    unsigned int saved = ct_wait_src_ready();
    ct_write32(ES_REG_SRC, (saved & CT_SRC_STATE_MASK) | CT_CODEC_SYNC_STATE);

    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        if ((ct_read32(ES_REG_SRC) & CT_CODEC_STATE_MASK) == 0) break;
        ct_io_delay();
    }
    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        if ((ct_read32(ES_REG_SRC) & CT_CODEC_STATE_MASK) ==
            CT_CODEC_SYNC_STATE) break;
        ct_io_delay();
    }
    return saved;
}

static void ct_leave_codec_window(unsigned int saved) {
    ct_wait_src_ready();
    ct_write32(ES_REG_SRC, saved);
}

static void ct_codec_write(unsigned char reg, unsigned short val) {
    unsigned int command;
    unsigned int saved;

    if (!g_ct_io_base) return;
    command = ((unsigned int)(reg & 0x7F) << 16) | val;
    if (g_ct_is_ev1938) command |= EV_1938_CODEC_MAGIC;

    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        if (!(ct_read32(ES_REG_CODEC) & ES_1371_CODEC_WIP)) {
            saved = ct_enter_codec_window();
            ct_write32(ES_REG_CODEC, command);
            ct_leave_codec_window(saved);
            return;
        }
        ct_io_delay();
    }
}

static unsigned short ct_codec_read(unsigned char reg) {
    unsigned int command;
    unsigned int saved;

    if (!g_ct_io_base) return 0;
    command = ((unsigned int)(reg & 0x7F) << 16) | ES_1371_CODEC_READ;
    if (g_ct_is_ev1938) command |= EV_1938_CODEC_MAGIC;

    for (int retry = 0; retry < 10; retry++) {
        int started = 0;
        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            if (!(ct_read32(ES_REG_CODEC) & ES_1371_CODEC_WIP)) {
                saved = ct_enter_codec_window();
                ct_write32(ES_REG_CODEC, command);
                ct_leave_codec_window(saved);
                started = 1;
                break;
            }
            ct_io_delay();
        }
        if (!started) continue;

        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            if (!(ct_read32(ES_REG_CODEC) & ES_1371_CODEC_WIP)) break;
            ct_io_delay();
        }
        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            unsigned int value = ct_read32(ES_REG_CODEC);
            if (value & ES_1371_CODEC_RDY) {
                if (g_ct_is_ev1938) {
                    for (int d = 0; d < 100; d++) (void)ct_read32(ES_REG_CONTROL);
                }
                return (unsigned short)(value & 0xFFFF);
            }
            ct_io_delay();
        }
    }
    return 0;
}

static void ct_program_src_rate(unsigned short base, int rate) {
    unsigned int freeze;
    unsigned int r;
    unsigned int freq;
    unsigned short int_regs;

    if (rate < 3000) rate = 3000;
    if (rate > 48000) rate = 48000;
    freq = (((unsigned int)rate << 15) + 1500U) / 3000U;

    if (base == ES_SMPREG_DAC2) {
        freeze = ES_1371_DIS_P2;
    } else if (base == ES_SMPREG_DAC1) {
        freeze = ES_1371_DIS_P1;
    } else {
        freeze = ES_1371_DIS_R1;
    }
    r = (ct_wait_src_ready() & CT_SRC_STATE_MASK) | freeze;
    ct_write32(ES_REG_SRC, r);

    int_regs = ct_src_read(base + ES_SMPREG_INT_REGS);
    ct_src_write(base + ES_SMPREG_INT_REGS,
                 (unsigned short)((int_regs & 0x00FF) | ((freq >> 5) & 0xFC00)));
    ct_src_write(base + ES_SMPREG_VFREQ_FRAC, (unsigned short)(freq & 0x7FFF));

    r = ct_wait_src_ready() & CT_SRC_STATE_MASK & ~freeze;
    ct_write32(ES_REG_SRC, r);
}

static int ct_needs_ac97_reset_quirk(unsigned short vendor,
                                     unsigned short device,
                                     unsigned char revision) {
    (void)vendor;
    if (device == CT5880_DEV_ID) {
        return revision == 0x02 || revision == 0x03 || revision == 0x04;
    }
    if (device == ES1371_DEV_ID) {
        return revision == 0x07 || revision == 0x08;
    }
    return 0;
}

int ct4810_init(void) {
    unsigned int status = 0;
    unsigned short vendor = 0;
    unsigned short device = 0;

    g_ct_present = 0;
    g_ct_io_base = 0;
    g_ct_chip_name = "None";
    g_ct_display_name[0] = '\0';
    g_ct_dma_running = 0;
    g_ct_revision = 0;
    g_ct_is_ev1938 = 0;

    for (int bus = 0; bus < 256 && !g_ct_present; bus++) {
        for (int dev = 0; dev < 32 && !g_ct_present; dev++) {
            for (int func = 0; func < 8; func++) {
                unsigned int id = pci_read_config(bus, dev, func, 0);
                if (id == 0xFFFFFFFF || id == 0) {
                    if (func == 0) break;
                    continue;
                }

                vendor = (unsigned short)(id & 0xFFFF);
                device = (unsigned short)(id >> 16);

                if (!((vendor == ENSONIQ_VENDOR_ID &&
                       (device == ES1371_DEV_ID || device == CT5880_DEV_ID)) ||
                      (vendor == CREATIVE_VENDOR_ID &&
                       (device == CT5880_DEV_ID || device == EV1938_DEV_ID)))) {
                    continue;
                }

                unsigned int bar0 = pci_read_config(bus, dev, func, 0x10);
                if (!(bar0 & 1)) continue;
                g_ct_io_base = (unsigned short)(bar0 & 0xFFFC);
                if (!g_ct_io_base) continue;

                unsigned int command = pci_read_config(bus, dev, func, 0x04);
                pci_write_config(bus, dev, func, 0x04, command | 0x07);

                unsigned int latency = pci_read_config(bus, dev, func, 0x0C);
                if ((latency & 0xFF00) < 0x4000) {
                    pci_write_config(bus, dev, func, 0x0C,
                                     (latency & 0xFFFF00FF) | 0x4000);
                }

                g_ct_revision =
                    (unsigned char)(pci_read_config(bus, dev, func, 0x08) & 0xFF);
                g_ct_is_ev1938 = (device == EV1938_DEV_ID);

                if (device == CT5880_DEV_ID) {
                    g_ct_chip_name = "Creative CT4810 (CT5880) PCI";
                } else if (g_ct_is_ev1938) {
                    g_ct_chip_name = "Creative AudioPCI (EV1938) PCI";
                } else {
                    g_ct_chip_name = "Ensoniq AudioPCI (ES1371) PCI";
                }
                g_ct_present = 1;
                break;
            }
        }
    }

    if (!g_ct_present || !g_ct_io_base) return -1;

    if (!g_ct_dma_buf) {
        g_ct_dma_buf = (unsigned char*)pmm_alloc_pages_aligned(16, 16);
        if (!g_ct_dma_buf) {
            g_ct_present = 0;
            return -2;
        }
        g_ct_dma_phys = (unsigned int)g_ct_dma_buf;
    }

    for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) g_ct_dma_buf[i] = 0;
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    // Keep the controller quiet while the codec and SRC are reset.
    ct_write32(ES_REG_CONTROL, 0);
    ct_write32(ES_REG_SERIAL, 0);
    ct_write32(ES_REG_LEGACY, 0);

    // Several CT5880/ES1373 revisions need the extra AC'97 reset pulse.
    if (ct_needs_ac97_reset_quirk(vendor, device, g_ct_revision)) {
        status |= ES_STAT_AC97_RST;
    }
    if (status) {
        ct_write32(ES_REG_STATUS, status);
        sleep_ms(20);
    }

    // Warm-reset the AC'97 link so the codec starts its bit clock.
    ct_write32(ES_REG_CONTROL, ES_CTL_SYNC_RES);
    (void)ct_read32(ES_REG_CONTROL);
    sleep_ms(2);
    ct_write32(ES_REG_CONTROL, 0);

    // Program all SRC parameters before enabling the converter. ALSA warns
    // that enabling an unconfigured SRC can leave the RAM busy forever.
    (void)ct_wait_src_ready();
    ct_write32(ES_REG_SRC, ES_1371_SRC_DISABLE);
    for (int i = 0; i < 0x80; i++) ct_src_write((unsigned short)i, 0);

    ct_src_write(ES_SMPREG_DAC1 + ES_SMPREG_TRUNC_N, 16 << 4);
    ct_src_write(ES_SMPREG_DAC1 + ES_SMPREG_INT_REGS, 16 << 10);
    ct_src_write(ES_SMPREG_DAC2 + ES_SMPREG_TRUNC_N, 16 << 4);
    ct_src_write(ES_SMPREG_DAC2 + ES_SMPREG_INT_REGS, 16 << 10);
    ct_src_write(ES_SMPREG_ADC + ES_SMPREG_TRUNC_N, 16 << 4);
    ct_src_write(ES_SMPREG_ADC + ES_SMPREG_INT_REGS, 16 << 10);
    ct_src_write(ES_SMPREG_VOL_ADC, 1 << 12);
    ct_src_write(ES_SMPREG_VOL_ADC + 1, 1 << 12);
    ct_src_write(ES_SMPREG_VOL_DAC1, 1 << 12);
    ct_src_write(ES_SMPREG_VOL_DAC1 + 1, 1 << 12);
    ct_src_write(ES_SMPREG_VOL_DAC2, 1 << 12);
    ct_src_write(ES_SMPREG_VOL_DAC2 + 1, 1 << 12);

    ct_program_src_rate(ES_SMPREG_DAC1, 44100);
    ct_program_src_rate(ES_SMPREG_DAC2, 44100);
    ct_program_src_rate(ES_SMPREG_ADC, 44100);
    (void)ct_wait_src_ready();
    ct_write32(ES_REG_SRC, 0);

    // Cold-reset the codec, then allow slower AK4540/TR28023 parts to wake.
    ct_codec_write(AC97_RESET, 0x0000);
    sleep_ms(50);
    unsigned short id1 = ct_codec_read(AC97_VENDOR_ID1);
    unsigned short id2 = ct_codec_read(AC97_VENDOR_ID2);
    if (id1 || id2) ct_set_codec_name(id1, id2);

    ct_codec_write(AC97_MASTER_VOLUME, 0x0000);
    ct_codec_write(AC97_HEADPHONE_VOLUME, 0x0000);
    ct_codec_write(AC97_PCM_OUT_VOLUME, 0x0000);
    ct_codec_write(0x0A, 0x8000); // Mute PC beep
    ct_codec_write(0x0E, 0x8008); // Mute microphone
    ct_codec_write(0x10, 0x8808); // Mute line input
    ct_codec_write(0x12, 0x8808); // Mute CD input

    ct4810_set_format(44100, 16, 1);

    ct_write32(ES_REG_MEMPAGE, ES_PAGE_DAC);
    ct_write32(ES_REG_DAC2_FRAME, g_ct_dma_phys);
    ct_write32(ES_REG_DAC2_SIZE, (CT_DMA_BUFFER_BYTES / 4) - 1);
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

    unsigned int mode = 0;
    if (bits == 16) mode |= 0x02;
    if (stereo) mode |= 0x01;

    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    sctrl &= ~(ES_P2_LOOP_SEL | ES_P2_PAUSE | ES_P2_DAC_SEN |
               ES_P2_END_INCM | ES_P2_ST_INCM | ES_P2_MODEM);
    sctrl |= ES_P2_INT_EN | ES_P2_MODEO(mode) |
             ES_P2_END_INCO(mode & 0x02 ? 2 : 1) | ES_P2_ST_INCO(0);
    ct_write32(ES_REG_SERIAL, sctrl);

    ct_program_src_rate(ES_SMPREG_DAC2, rate);
}

void ct4810_start_dma(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_buf) return;

    ct_write32(ES_REG_MEMPAGE, ES_PAGE_DAC);
    ct_write32(ES_REG_DAC2_FRAME, g_ct_dma_phys);
    ct_write32(ES_REG_DAC2_SIZE, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(ES_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);

    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    sctrl &= ~(ES_P2_LOOP_SEL | ES_P2_PAUSE);
    ct_write32(ES_REG_SERIAL, sctrl);

    unsigned int control = ct_read32(ES_REG_CONTROL);
    control &= ~ES_CTL_BYPASS_P2;
    control |= ES_CTL_BREQ | ES_CTL_DAC2_EN;
    ct_write32(ES_REG_CONTROL, control);

    g_ct_dma_running = 1;
    g_ct_last_half_playing = 0;
    g_ct_last_toggle_tick = global_tick;
    g_ct_last_seen_frame = 0xFFFFFFFF;
    g_ct_hw_reg_moving = 0;
}

void ct4810_stop_dma(void) {
    if (!g_ct_present || !g_ct_io_base) return;

    unsigned int control = ct_read32(ES_REG_CONTROL);
    control &= ~(ES_CTL_BREQ | ES_CTL_DAC2_EN);
    ct_write32(ES_REG_CONTROL, control);

    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    ct_write32(ES_REG_SERIAL, sctrl | ES_P2_PAUSE);

    g_ct_dma_running = 0;
    g_ct_last_half_playing = -1;
    g_ct_last_toggle_tick = 0;
    g_ct_last_seen_frame = 0xFFFFFFFF;
    g_ct_hw_reg_moving = 0;
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    if (g_ct_dma_buf) {
        for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) g_ct_dma_buf[i] = 0;
    }
}

void ct4810_pause_dma(int pause) {
    if (!g_ct_present || !g_ct_io_base) return;
    unsigned int sctrl = ct_read32(ES_REG_SERIAL);
    if (pause) sctrl |= ES_P2_PAUSE;
    else sctrl &= ~ES_P2_PAUSE;
    ct_write32(ES_REG_SERIAL, sctrl);
}

void ct4810_set_volume(int master_vol, int pcm_vol) {
    if (!g_ct_present || !g_ct_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    unsigned char m_att = (unsigned char)((100 - master_vol) * 31 / 100);
    unsigned char p_att = (unsigned char)((100 - pcm_vol) * 31 / 100);
    unsigned short m_val =
        (master_vol == 0) ? 0x8000 : (unsigned short)((m_att << 8) | m_att);
    unsigned short p_val =
        (pcm_vol == 0) ? 0x8000 : (unsigned short)((p_att << 8) | p_att);

    ct_codec_write(AC97_MASTER_VOLUME, m_val);
    ct_codec_write(AC97_HEADPHONE_VOLUME, m_val);
    ct_codec_write(AC97_PCM_OUT_VOLUME, p_val);
}

unsigned int ct4810_get_dma_frame_pos(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return 0;

    ct_write32(ES_REG_MEMPAGE, ES_PAGE_DAC);
    unsigned int val = ct_read32(ES_REG_DAC2_SIZE);
    unsigned int frame = (val >> 16) & 0xFFFF;
    if (frame >= CT_DMA_TOTAL_FRAMES) frame = 0;
    return frame;
}

const void* ct4810_get_dma_buffer(void) {
    return g_ct_dma_buf;
}

void ct4810_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_ct_dma_buf) return;
    if (half_idx < 0 || half_idx > 1 || !data) return;
    if (bytes > CT_DMA_HALF_BYTES) bytes = CT_DMA_HALF_BYTES;

    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    const unsigned char* src = (const unsigned char*)data;
    for (unsigned int i = 0; i < bytes; i++) dest[i] = src[i];
    for (unsigned int i = bytes; i < CT_DMA_HALF_BYTES; i++) dest[i] = 0;

    g_ct_half_filled[half_idx] = 1;
}

void ct4810_silence_half(int half_idx) {
    if (!g_ct_dma_buf || half_idx < 0 || half_idx > 1) return;
    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    for (unsigned int i = 0; i < CT_DMA_HALF_BYTES; i++) dest[i] = 0;
    g_ct_half_filled[half_idx] = 0;
}

int ct4810_poll_half_needed(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return -1;

    unsigned int frame = ct4810_get_dma_frame_pos();
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
        if (!g_ct_half_filled[other_half]) return other_half;
        return -1;
    }

    // Keep progress moving if a virtualized card does not implement its
    // position register. Real cards normally take the path above.
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
    if (half_idx >= 0 && half_idx < 2) g_ct_half_filled[half_idx] = 1;
}
