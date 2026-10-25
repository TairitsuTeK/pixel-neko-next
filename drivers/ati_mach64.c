#include "ati_mach64.h"
#include "port.h"

// ----------------------------------------------------------------------------
// ATI Mach64 / 3D Rage II+DVD 2D hardware driver.
// Register layouts follow Linux atyfb and the Mach64 register reference.
// ----------------------------------------------------------------------------

static int g_ati_active = 0;
static const char* g_ati_chip_name = "None";
static unsigned int g_ati_vram_bytes = 2 * 1024 * 1024;
static volatile unsigned char* g_ati_regs = 0;
static volatile unsigned char* g_ati_lfb = 0;
static int g_cur_w = 800, g_cur_h = 600;

static inline unsigned int mach64_read32(unsigned int reg) {
    if (!g_ati_regs) return 0xFFFFFFFF;
    return *(volatile unsigned int*)(g_ati_regs + reg);
}

static inline void mach64_write32(unsigned int reg, unsigned int val) {
    if (!g_ati_regs) return;
    *(volatile unsigned int*)(g_ati_regs + reg) = val;
}

static unsigned int mach64_bytes_per_pixel(int bpp) {
    if (bpp <= 8) return 1;
    if (bpp <= 16) return 2;
    if (bpp <= 24) return 3;
    return 4;
}

static unsigned int mach64_pitch_bytes(int w, int bpp) {
    unsigned int bytes = (unsigned int)w * mach64_bytes_per_pixel(bpp);
    return (bytes + 7U) & ~7U;
}

static unsigned int mach64_pitch_units(int w) {
    return (((unsigned int)w + 7U) & ~7U) / 8U;
}

static unsigned int mach64_pixel_code(int bpp) {
    if (bpp <= 8) return 2;
    if (bpp == 15) return 3;
    if (bpp == 16) return 4;
    if (bpp == 24) return 5;
    return 6;
}

static int mach64_regs_match(unsigned int base, unsigned short device) {
    if (!base) return 0;
    volatile unsigned int* regs = (volatile unsigned int*)base;
    unsigned int chip_id = regs[MACH64_CNFG_CHIP_ID / 4];
    unsigned int h_total = regs[0];
    if (chip_id == 0xFFFFFFFF || h_total == 0xFFFFFFFF) return 0;
    return (unsigned short)(chip_id & 0xFFFF) == device;
}

static unsigned int mach64_find_register_base(unsigned int bar0,
                                              unsigned int bar2,
                                              unsigned short device) {
    // The Rage II+ exposes block 1 at the auxiliary BAR and block 0 at +0x400.
    if (bar2 && !(bar2 & 1)) {
        unsigned int base = (bar2 & 0xFFFFFFF0U) + 0x400;
        if (mach64_regs_match(base, device)) return base;
    }

    // Packed Mach64 register aperture: block 0 is at the end of the 8MB (or
    // fallback 4MB) linear aperture.
    unsigned int base8 = bar0 + 0x7FFC00U;
    if (mach64_regs_match(base8, device)) return base8;

    unsigned int base4 = bar0 + 0x3FFC00U;
    if (mach64_regs_match(base4, device)) return base4;
    return 0;
}

static inline void wait_fifo(int entries) {
    if (!g_ati_regs) return;
    int timeout = 100000;
    while (((mach64_read32(MACH64_FIFO_STAT) & 0xFFFF) <=
            (unsigned int)(0x8000 >> entries)) && --timeout) {
    }
}

void ati_mach64_sync(void) {
    if (!g_ati_regs || !g_ati_active) return;
    int timeout = 200000;
    while ((mach64_read32(MACH64_GUI_STAT) & 1) && --timeout) {
    }
}

int ati_mach64_probe(void) {
    g_ati_active = 0;
    g_ati_regs = 0;
    g_ati_lfb = 0;

    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            unsigned int id = pci_read_config(bus, slot, 0, 0);
            if (id == 0xFFFFFFFF || id == 0) continue;

            unsigned short vendor = id & 0xFFFF;
            unsigned short device = (id >> 16) & 0xFFFF;
            if (vendor != ATI_VENDOR_ID) continue;

            const char* name = 0;
            switch (device) {
                case PCI_DEVICE_ATI_RAGE_GT:      name = "ATI 3D Rage (Mach64 GT)"; break;
                case PCI_DEVICE_ATI_RAGE_II:      name = "ATI 3D Rage II+DVD (Mach64 GT-B)"; break;
                case PCI_DEVICE_ATI_RAGE_IIC_PCI: name = "ATI 3D Rage IIc (PCI)"; break;
                case PCI_DEVICE_ATI_RAGE_IIC_AGP: name = "ATI 3D Rage IIc (AGP)"; break;
                case PCI_DEVICE_ATI_RAGE_IIC_B:
                case PCI_DEVICE_ATI_RAGE_IIC_C:   name = "ATI 3D Rage IIC"; break;
                case PCI_DEVICE_ATI_RAGE_PRO_PCI:
                case PCI_DEVICE_ATI_RAGE_PRO_TRB:
                case PCI_DEVICE_ATI_RAGE_PRO_AGP: name = "ATI 3D Rage Pro"; break;
                case PCI_DEVICE_ATI_RAGE_XL:
                case PCI_DEVICE_ATI_RAGE_XL_PCI:  name = "ATI Rage XL"; break;
                case PCI_DEVICE_ATI_MACH64_VT:
                case PCI_DEVICE_ATI_MACH64_VT4:   name = "ATI Mach64 VT"; break;
                default: break;
            }
            if (!name) continue;

            unsigned int raw_bar0 = pci_read_config(bus, slot, 0, 0x10);
            unsigned int raw_bar2 = pci_read_config(bus, slot, 0, 0x18);
            unsigned int bar0 = raw_bar0 & 0xFFFFFFF0U;
            unsigned int bar2 = 0;
            if (raw_bar2 && raw_bar2 != 0xFFFFFFFFU && !(raw_bar2 & 1)) {
                bar2 = raw_bar2 & 0xFFFFFFF0U;
            }
            if (!bar0) return 0;

            unsigned int cmd = pci_read_config(bus, slot, 0, 0x04);
            pci_write_config(bus, slot, 0, 0x04, cmd | 0x07);

            g_ati_chip_name = name;
            g_ati_lfb = (volatile unsigned char*)bar0;

            unsigned int reg_base = mach64_find_register_base(bar0, bar2, device);
            g_ati_regs = reg_base ? (volatile unsigned char*)reg_base : 0;

            // Query VBE BIOS for the authoritative physical VRAM size.
            extern int vbe_get_controller_info(void);
            unsigned int vbe_vram = 0;
            if (vbe_get_controller_info() == 0x004F) {
                unsigned char* vbe_block = (unsigned char*)0x8800;
                if (vbe_block[0] == 'V' && vbe_block[1] == 'E' &&
                    vbe_block[2] == 'S' && vbe_block[3] == 'A') {
                    unsigned short blocks =
                        *(unsigned short*)(vbe_block + 18);
                    if (blocks > 0) {
                        vbe_vram = (unsigned int)blocks * 64 * 1024;
                    }
                }
            }

            // Probe the framebuffer aperture at 1, 2, 4 and 8MB.
            volatile unsigned int* p0 = (volatile unsigned int*)(g_ati_lfb);
            volatile unsigned int* p1 =
                (volatile unsigned int*)(g_ati_lfb + 1 * 1024 * 1024);
            volatile unsigned int* p2 =
                (volatile unsigned int*)(g_ati_lfb + 2 * 1024 * 1024);
            volatile unsigned int* p4 =
                (volatile unsigned int*)(g_ati_lfb + 4 * 1024 * 1024);

            unsigned int save0 = *p0;
            unsigned int save1 = *p1;
            unsigned int save2 = *p2;
            unsigned int save4 = *p4;

            *p0 = 0xA55A1234;
            *p1 = 0x5AA54321;
            if (*p1 != 0x5AA54321 || *p0 == 0x5AA54321) {
                g_ati_vram_bytes = 1 * 1024 * 1024;
            } else {
                *p2 = 0x5AA5789A;
                if (*p2 != 0x5AA5789A || *p0 == 0x5AA5789A) {
                    g_ati_vram_bytes = 2 * 1024 * 1024;
                } else {
                    *p4 = 0x5AA5BCDE;
                    if (*p4 != 0x5AA5BCDE || *p0 == 0x5AA5BCDE) {
                        g_ati_vram_bytes = 4 * 1024 * 1024;
                    } else {
                        g_ati_vram_bytes = 8 * 1024 * 1024;
                    }
                }
            }

            *p0 = save0;
            *p1 = save1;
            *p2 = save2;
            *p4 = save4;

            if (vbe_vram > 0 && vbe_vram <= 8 * 1024 * 1024) {
                g_ati_vram_bytes = vbe_vram;
            }

            if (g_ati_regs) {
                mach64_write32(MACH64_GEN_TEST_CNTL,
                               mach64_read32(MACH64_GEN_TEST_CNTL) | (1 << 8));
                ati_mach64_sync();
                mach64_write32(MACH64_GEN_TEST_CNTL,
                               mach64_read32(MACH64_GEN_TEST_CNTL) & ~(1 << 8));
            }

            g_ati_active = 1;
            return 1;
        }
    }
    return 0;
}

int ati_mach64_is_active(void) {
    return g_ati_active;
}

const char* ati_mach64_get_chip_name(void) {
    return g_ati_chip_name;
}

unsigned int ati_mach64_get_vram_size(void) {
    return g_ati_vram_bytes;
}

int ati_mach64_supports_native_32bpp(int w, int h) {
    if (!g_ati_active || !g_ati_regs || w <= 0 || h <= 0) return 0;
    if (w & 7) return 0;
    if (mach64_pitch_units(w) > 0x3FF) return 0;
    return (unsigned int)w * (unsigned int)h * 4 <= g_ati_vram_bytes;
}

int ati_mach64_enable_native_32bpp(int w, int h) {
    if (!ati_mach64_supports_native_32bpp(w, h)) return 0;

    ati_mach64_sync();

    unsigned int off_pitch = mach64_read32(MACH64_CRTC_OFF_PITCH);
    unsigned int gen_cntl = mach64_read32(MACH64_CRTC_GEN_CNTL);
    if (off_pitch == 0xFFFFFFFF || gen_cntl == 0xFFFFFFFF) return 0;

    unsigned int pitch_units = mach64_pitch_units(w);
    off_pitch &= 0x000FFFFF;
    off_pitch |= (pitch_units & 0x3FF) << 22;
    mach64_write32(MACH64_CRTC_OFF_PITCH, off_pitch);

    gen_cntl &= ~MACH64_PIX_WIDTH_MASK;
    gen_cntl |= MACH64_PIX_WIDTH_32BPP;
    mach64_write32(MACH64_CRTC_GEN_CNTL, gen_cntl);

    // Integrated Mach64/Rage parts use MEM_CNTL bits 27:20 for the memory
    // pixel mode. 32bpp selects the same mode Linux atyfb calls 0x08.
    unsigned int mem_cntl = mach64_read32(MACH64_MEM_CNTL);
    mem_cntl &= 0xF00FFFFFU;
    mem_cntl |= 0x08000000U;
    mach64_write32(MACH64_MEM_CNTL, mem_cntl);

    ati_mach64_set_mode_bpp(w, h, 32);

    return (mach64_read32(MACH64_CRTC_GEN_CNTL) & MACH64_PIX_WIDTH_MASK) ==
           MACH64_PIX_WIDTH_32BPP;
}

void ati_mach64_set_mode_bpp(int w, int h, int bpp) {
    if (!g_ati_active) return;

    g_cur_w = w;
    g_cur_h = h;
    if (!g_ati_regs) return;

    ati_mach64_sync();

    unsigned int pitch_units = mach64_pitch_units(w);
    mach64_write32(MACH64_DST_OFF_PITCH, (pitch_units & 0x3FF) << 22);
    mach64_write32(MACH64_SRC_OFF_PITCH, (pitch_units & 0x3FF) << 22);

    unsigned int code = mach64_pixel_code(bpp);
    mach64_write32(MACH64_DP_PIX_WIDTH,
                   code | (code << 8) | (code << 16) | (1U << 24));

    unsigned int chain_mask = 0x8080;
    if (bpp == 15) chain_mask = 0x4210;
    else if (bpp == 16) chain_mask = 0x8410;
    mach64_write32(MACH64_DP_CHAIN_MASK, chain_mask);

    mach64_write32(MACH64_SC_LEFT_RIGHT, ((unsigned int)(w - 1) & 0x3FFF) << 16);
    mach64_write32(MACH64_SC_TOP_BOTTOM, ((unsigned int)(h - 1) & 0x7FFF) << 16);
    mach64_write32(MACH64_DP_MIX, 0x00070007);
}

void ati_mach64_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (!g_ati_active || !g_ati_regs) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_cur_w) w = g_cur_w - x;
    if (y + h > g_cur_h) h = g_cur_h - y;
    if (w <= 0 || h <= 0) return;

    wait_fifo(5);
    mach64_write32(MACH64_DP_FRGD_CLR, color);
    mach64_write32(MACH64_DP_SRC, 0x00000100);
    mach64_write32(MACH64_DP_MIX, 0x00070007);
    mach64_write32(MACH64_DST_Y_X, ((unsigned int)x << 16) | (y & 0xFFFF));
    mach64_write32(MACH64_DST_WIDTH_HEIGHT,
                   ((unsigned int)w << 16) | (h & 0xFFFF));
}

void ati_mach64_screen_blt(int sx, int sy, int dx, int dy, int w, int h) {
    if (!g_ati_active || !g_ati_regs) return;

    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > g_cur_w) w = g_cur_w - sx;
    if (sy + h > g_cur_h) h = g_cur_h - sy;
    if (dx + w > g_cur_w) w = g_cur_w - dx;
    if (dy + h > g_cur_h) h = g_cur_h - dy;
    if (w <= 0 || h <= 0) return;

    unsigned int traj = 0x0003;
    if (dy > sy) {
        traj &= ~0x0002;
        sy += h - 1;
        dy += h - 1;
    }
    if (dx > sx) {
        traj &= ~0x0001;
        sx += w - 1;
        dx += w - 1;
    }

    wait_fifo(5);
    mach64_write32(MACH64_GUI_TRAJ_CNTL, traj);
    mach64_write32(MACH64_DP_SRC, 0x00000300);
    mach64_write32(MACH64_DP_MIX, 0x00070007);
    mach64_write32(MACH64_SRC_Y_X,
                   ((unsigned int)sx << 16) | (sy & 0xFFFF));
    mach64_write32(MACH64_DST_Y_X,
                   ((unsigned int)dx << 16) | (dy & 0xFFFF));
    mach64_write32(MACH64_DST_WIDTH_HEIGHT,
                   ((unsigned int)w << 16) | (h & 0xFFFF));
}
