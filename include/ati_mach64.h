#ifndef ATI_MACH64_H
#define ATI_MACH64_H

// ----------------------------------------------------------------------------
// ATI Mach64 / 3D Rage II+ Hardware Register Definitions
// Ported from Linux Kernel atyfb (drivers/video/fbdev/aty/atyfb.h) & XFree86
// ----------------------------------------------------------------------------

#define ATI_VENDOR_ID               0x1002

// Supported PCI Device IDs (Mach64 / 3D Rage Family)
#define PCI_DEVICE_ATI_RAGE_GT      0x4754  // 3D Rage (Mach64 GT)
#define PCI_DEVICE_ATI_RAGE_II      0x4755  // 3D Rage II / II+DVD (Mach64 GT-B)
#define PCI_DEVICE_ATI_RAGE_IIC_PCI 0x4756  // 3D Rage IIc PCI
#define PCI_DEVICE_ATI_RAGE_IIC_AGP 0x4757  // 3D Rage IIc AGP
#define PCI_DEVICE_ATI_RAGE_IIC_B   0x4759  // 3D Rage IIC
#define PCI_DEVICE_ATI_RAGE_IIC_C   0x475A  // 3D Rage IIC
#define PCI_DEVICE_ATI_RAGE_PRO_PCI 0x4742  // 3D Rage Pro PCI
#define PCI_DEVICE_ATI_RAGE_PRO_TRB 0x4744  // 3D Rage Pro Turbo
#define PCI_DEVICE_ATI_RAGE_PRO_AGP 0x4749  // 3D Rage Pro AGP
#define PCI_DEVICE_ATI_RAGE_XL      0x474D  // Rage XL
#define PCI_DEVICE_ATI_RAGE_XL_PCI  0x4752  // Rage XL PCI
#define PCI_DEVICE_ATI_MACH64_VT    0x5654  // Mach64 VT
#define PCI_DEVICE_ATI_MACH64_VT4   0x5655  // Mach64 VT4

// Mach64 Block 0 MMIO Register Offsets (relative to BAR1 MMIO Base)
#define MACH64_DST_OFF_PITCH        0x0000
#define MACH64_DST_X                0x0004
#define MACH64_DST_Y                0x0008
#define MACH64_DST_Y_X              0x0008
#define MACH64_DST_WIDTH            0x000C
#define MACH64_DST_HEIGHT           0x0010
#define MACH64_DST_WIDTH_HEIGHT     0x000C
#define MACH64_SRC_OFF_PITCH        0x0014
#define MACH64_SRC_X                0x0018
#define MACH64_SRC_Y                0x001C
#define MACH64_SRC_Y_X              0x0010
#define MACH64_SRC_WIDTH1           0x0014
#define MACH64_GUI_TRAJ_CNTL        0x0018
#define MACH64_DP_SRC               0x0020
#define MACH64_DP_FRGD_CLR          0x0024
#define MACH64_DP_BKGD_CLR          0x0028
#define MACH64_DP_MIX               0x0028
#define MACH64_DP_DATATYPE          0x002C
#define MACH64_CLR_CMP_CNTL         0x0030
#define MACH64_FIFO_STAT            0x0038
#define MACH64_GUI_STAT             0x0038

#define MACH64_CONFIG_STAT0         0x00A0
#define MACH64_MEM_CNTL             0x00B0
#define MACH64_DAC_REGS             0x00C0
#define MACH64_DAC_CNTL             0x00C4
#define MACH64_GEN_TEST_CNTL        0x00D4
#define MACH64_CRTC_GEN_CNTL        0x00EC

// ----------------------------------------------------------------------------
// Driver API
// ----------------------------------------------------------------------------
int          ati_mach64_probe(void);
int          ati_mach64_is_active(void);
const char*  ati_mach64_get_chip_name(void);
unsigned int ati_mach64_get_vram_size(void);
void         ati_mach64_set_mode_bpp(int w, int h, int bpp);
void         ati_mach64_fill_rect(int x, int y, int w, int h, unsigned int color);
void         ati_mach64_screen_blt(int sx, int sy, int dx, int dy, int w, int h);
void         ati_mach64_sync(void);
void         ati_mach64_program_ramdac(int bpp);

#endif
