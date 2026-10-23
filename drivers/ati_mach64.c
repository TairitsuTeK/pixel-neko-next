#include "ati_mach64.h"
#include "port.h"

// ----------------------------------------------------------------------------
// ATI Mach64 / 3D Rage II+DVD 2D Hardware Acceleration Driver
// Ported from Linux atyfb (drivers/video/fbdev/aty/atyfb_base.c & mach64_accel.c)
// ----------------------------------------------------------------------------

static int g_ati_active = 0;
static unsigned short g_ati_device_id = 0;
static const char* g_ati_chip_name = "None";
static unsigned int g_ati_vram_bytes = 2 * 1024 * 1024; // Default 2MB
static volatile unsigned char* g_ati_mmio = 0;
static volatile unsigned char* g_ati_lfb = 0;
static int g_cur_w = 800, g_cur_h = 600, g_cur_bpp = 8;
static unsigned int g_cur_pitch = 800;

static inline unsigned int mmio_read32(unsigned int reg) {
    if (!g_ati_mmio) return 0;
    return *(volatile unsigned int*)(g_ati_mmio + reg);
}

static inline void mmio_write32(unsigned int reg, unsigned int val) {
    if (!g_ati_mmio) return;
    *(volatile unsigned int*)(g_ati_mmio + reg) = val;
}

static inline void wait_fifo(int entries) {
    if (!g_ati_mmio) return;
    int timeout = 100000;
    while (((mmio_read32(MACH64_FIFO_STAT) & 0xFFFF) <= (0x8000 >> entries)) && --timeout);
}

void ati_mach64_sync(void) {
    if (!g_ati_mmio || !g_ati_active) return;
    int timeout = 200000;
    while ((mmio_read32(MACH64_GUI_STAT) & 1) && --timeout);
}

int ati_mach64_probe(void) {
    g_ati_active = 0;
    g_ati_mmio = 0;
    g_ati_lfb = 0;

    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            unsigned int id = pci_read_config(bus, slot, 0, 0);
            if (id == 0xFFFFFFFF || id == 0) continue;

            unsigned short vendor = id & 0xFFFF;
            unsigned short device = (id >> 16) & 0xFFFF;

            if (vendor == ATI_VENDOR_ID) {
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

                if (name) {
                    g_ati_device_id = device;
                    g_ati_chip_name = name;

                    // Read BAR0 (LFB) and BAR1 (MMIO)
                    unsigned int bar0 = pci_read_config(bus, slot, 0, 0x10) & 0xFFFFFFF0;
                    unsigned int bar1 = pci_read_config(bus, slot, 0, 0x14) & 0xFFFFFFF0;

                    if (!bar0 || !bar1) return 0; // Invalid BARs -> fallback

                    // Enable PCI Bus Mastering, Memory Space, and I/O Space
                    unsigned int cmd = pci_read_config(bus, slot, 0, 0x04);
                    pci_write_config(bus, slot, 0, 0x04, cmd | 0x07);

                    g_ati_lfb = (volatile unsigned char*)bar0;
                    g_ati_mmio = (volatile unsigned char*)bar1;

                    // Probe physical VRAM size from MEM_CNTL
                    unsigned int mem_cntl = mmio_read32(MACH64_MEM_CNTL);
                    unsigned int mem_type = mem_cntl & 0x07;
                    switch (mem_type) {
                        case 0: g_ati_vram_bytes = 512 * 1024; break;
                        case 1: g_ati_vram_bytes = 1024 * 1024; break;
                        case 2: g_ati_vram_bytes = 2 * 1024 * 1024; break;
                        case 3: g_ati_vram_bytes = 4 * 1024 * 1024; break;
                        case 4: g_ati_vram_bytes = 6 * 1024 * 1024; break;
                        case 5: g_ati_vram_bytes = 8 * 1024 * 1024; break;
                        default: g_ati_vram_bytes = 2 * 1024 * 1024; break;
                    }

                    // Reset 2D GUI Engine test/control
                    mmio_write32(MACH64_GEN_TEST_CNTL, mmio_read32(MACH64_GEN_TEST_CNTL) | (1 << 8));
                    ati_mach64_sync();
                    mmio_write32(MACH64_GEN_TEST_CNTL, mmio_read32(MACH64_GEN_TEST_CNTL) & ~(1 << 8));

                    g_ati_active = 1;
                    return 1;
                }
            }
        }
    }

    return 0; // Not found -> smooth fallback to generic VBE
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

void ati_mach64_program_ramdac(int bpp) {
    if (!g_ati_active || !g_ati_mmio) return;

    // Unlock DAC_VGA_ADR_EN (bit 13) in DAC_CNTL
    unsigned int dac_cntl = mmio_read32(MACH64_DAC_CNTL);
    dac_cntl |= (1 << 13);
    
    // Set 8bpp / 16bpp / 24bpp / 32bpp DAC mode in DAC_CNTL
    if (bpp == 32) {
        dac_cntl |= (3 << 8); // 32bpp direct color mode
    } else if (bpp == 24) {
        dac_cntl |= (2 << 8); // 24bpp direct color mode
    } else if (bpp == 16) {
        dac_cntl |= (1 << 8); // 16bpp direct color mode
    }
    mmio_write32(MACH64_DAC_CNTL, dac_cntl);

    // Write linear identity ramp to internal RAMDAC LUT (256 entries)
    *(volatile unsigned char*)(g_ati_mmio + MACH64_DAC_REGS) = 0; // Index 0
    for (int i = 0; i < 256; i++) {
        unsigned char val = (unsigned char)(bpp == 8 ? (i >> 2) : i);
        *(volatile unsigned char*)(g_ati_mmio + MACH64_DAC_REGS + 1) = val; // R
        *(volatile unsigned char*)(g_ati_mmio + MACH64_DAC_REGS + 1) = val; // G
        *(volatile unsigned char*)(g_ati_mmio + MACH64_DAC_REGS + 1) = val; // B
    }
}

void ati_mach64_set_mode_bpp(int w, int h, int bpp) {
    if (!g_ati_active || !g_ati_mmio) return;

    g_cur_w = w;
    g_cur_h = h;
    g_cur_bpp = bpp;
    g_cur_pitch = w;

    ati_mach64_sync();

    // Configure destination and source pitch
    unsigned int pitch_units = (w >> 3) & 0x3FF;
    mmio_write32(MACH64_DST_OFF_PITCH, (pitch_units << 22));
    mmio_write32(MACH64_SRC_OFF_PITCH, (pitch_units << 22));

    // Configure DP_DATATYPE for hardware 2D engine
    unsigned int datatype = 2; // 8bpp
    if (bpp == 15) datatype = 3;
    else if (bpp == 16) datatype = 4;
    else if (bpp == 24) datatype = 5;
    else if (bpp == 32) datatype = 6;
    mmio_write32(MACH64_DP_DATATYPE, (datatype << 8));

    // Default ROP: Source copy (MIX_SRC)
    mmio_write32(MACH64_DP_MIX, 0x00070007);

    // Program RAMDAC LUT
    ati_mach64_program_ramdac(bpp);
}

void ati_mach64_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (!g_ati_active || !g_ati_mmio) return;

    // Clipping to screen bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_cur_w) w = g_cur_w - x;
    if (y + h > g_cur_h) h = g_cur_h - y;
    if (w <= 0 || h <= 0) return;

    wait_fifo(5);
    mmio_write32(MACH64_DP_FRGD_CLR, color);
    mmio_write32(MACH64_DP_SRC, 0x00000100); // DP_SRC_FRGD
    mmio_write32(MACH64_DP_MIX, 0x00070007); // MIX_SRC
    mmio_write32(MACH64_DST_Y_X, (x << 16) | (y & 0xFFFF));
    mmio_write32(MACH64_DST_WIDTH_HEIGHT, (w << 16) | (h & 0xFFFF)); // Trigger draw
}

void ati_mach64_screen_blt(int sx, int sy, int dx, int dy, int w, int h) {
    if (!g_ati_active || !g_ati_mmio) return;

    // Clipping
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > g_cur_w) w = g_cur_w - sx;
    if (sy + h > g_cur_h) h = g_cur_h - sy;
    if (dx + w > g_cur_w) w = g_cur_w - dx;
    if (dy + h > g_cur_h) h = g_cur_h - dy;
    if (w <= 0 || h <= 0) return;

    // Determine blit direction to avoid overlap tearing
    unsigned int traj = 0x0003; // Left to right, top to bottom
    if (dy > sy) {
        traj &= ~0x0002; // Bottom to top
        sy += h - 1;
        dy += h - 1;
    }
    if (dx > sx) {
        traj &= ~0x0001; // Right to left
        sx += w - 1;
        dx += w - 1;
    }

    wait_fifo(5);
    mmio_write32(MACH64_GUI_TRAJ_CNTL, traj);
    mmio_write32(MACH64_DP_SRC, 0x00000300); // DP_SRC_HOST / VRAM
    mmio_write32(MACH64_DP_MIX, 0x00070007); // MIX_SRC
    mmio_write32(MACH64_SRC_Y_X, (sx << 16) | (sy & 0xFFFF));
    mmio_write32(MACH64_DST_Y_X, (dx << 16) | (dy & 0xFFFF));
    mmio_write32(MACH64_DST_WIDTH_HEIGHT, (w << 16) | (h & 0xFFFF)); // Trigger draw
}
