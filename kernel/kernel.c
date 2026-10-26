#include "port.h"
#include "mouse.h"
#include "wm.h"
#include "keyboard.h"
#include "timer.h"
#include "speaker.h"
#include "audio.h"
#include "cpu.h"
#include "splash.h"
#include "settings.h"
#include "ati_mach64.h"
#include "gfx_accel.h"
#include "pci.h"
#include "irq.h"
#include "log.h"
#include "version.h"
#include "usb_core.h"

int screen_w = 320;
int screen_h = 200;
int screen_bpp = 8;
int screen_pitch = 320;
unsigned char backbuffer[1024 * 768 * 4];
unsigned int lfb_ptr = 0xA0000;
unsigned char* lfb = (unsigned char*)0xA0000;
unsigned short palette16[256];
unsigned int palette32[256];

extern int vbe_set_mode(int mode);
extern int vbe_get_mode_info(int mode);
extern int vbe_get_controller_info(void);

struct vbe_mode_info {
    short attributes;
    char winA, winB;
    short granularity;
    short win_size;
    short segmentA, segmentB;
    int win_func_ptr;
    short bytes_per_scanline;
    short x_res, y_res;
    char x_char_size, y_char_size;
    char planes, bpp, banks;
    char memory_model, bank_size, image_pages;
    char reserved1;
    char red_mask, red_pos;
    char green_mask, green_pos;
    char blue_mask, blue_pos;
    char reserved_mask, reserved_pos;
    char direct_color_attributes;
    int phys_base_ptr;
    int reserved2;
    short reserved3;
} __attribute__((packed));

extern void check_revert_timer(void);

#ifndef NULL
#define NULL ((void*)0)
#endif

#include "idt.h"
#include "mm.h"

extern void bios_set_mode(int mode);

static const pci_device_t* pci_vga;

static void pci_scan_vga(void)
{
    pci_vga = 0;
    pci_scan_all();
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (dev && dev->class_code == 0x03) {
            pci_vga = dev;
            pci_enable_io(dev);
            pci_enable_memory(dev);
            pci_enable_bus_master(dev);
            log_printf("[VGA ] %X:%X bus=%u slot=%u func=%u bar0=0x%X\n",
                       dev->vendor, dev->device, dev->bus, dev->slot,
                       dev->function, dev->bar[0]);
            return;
        }
    }
    log_write("[VGA ] no PCI display controller found\n");
}

static void s3_enable_law(void)
{
    // Unlock S3 registers
    outb(0x3D4, 0x38);
    outb(0x3D5, 0x48);
    outb(0x3D4, 0x39);
    outb(0x3D5, 0xA5);
    
    // CRTC 0x58: Linear Address Window (LAW) Control
    // Bit 4: Enable LAW; Bits 0-1: Window size (3 = 4MB/8MB)
    outb(0x3D4, 0x58);
    unsigned char cr58 = inb(0x3D5);
    outb(0x3D4, 0x58);
    outb(0x3D5, cr58 | 0x13);
}

static const unsigned char vga16[16][3] = {
    {0x00,0x00,0x00}, {0x00,0x00,0x2A}, {0x00,0x2A,0x00}, {0x00,0x2A,0x2A},
    {0x2A,0x00,0x00}, {0x2A,0x00,0x2A}, {0x2A,0x15,0x00}, {0x2A,0x2A,0x2A},
    {0x15,0x15,0x15}, {0x15,0x15,0x3F}, {0x15,0x3F,0x15}, {0x15,0x3F,0x3F},
    {0x3F,0x15,0x15}, {0x3F,0x15,0x3F}, {0x3F,0x3F,0x15}, {0x3F,0x3F,0x3F}
};

static unsigned short make_color16(int r, int g, int b, const struct vbe_mode_info* info)
{
    if (info && (unsigned char)info->red_mask > 0 && (unsigned char)info->green_mask > 0 && (unsigned char)info->blue_mask > 0) {
        unsigned char r_mask = (unsigned char)info->red_mask;
        unsigned char g_mask = (unsigned char)info->green_mask;
        unsigned char b_mask = (unsigned char)info->blue_mask;
        unsigned char r_pos = (unsigned char)info->red_pos;
        unsigned char g_pos = (unsigned char)info->green_pos;
        unsigned char b_pos = (unsigned char)info->blue_pos;

        int r_val = ((r >> (8 - r_mask)) & ((1 << r_mask) - 1)) << r_pos;
        int g_val = ((g >> (8 - g_mask)) & ((1 << g_mask) - 1)) << g_pos;
        int b_val = ((b >> (8 - b_mask)) & ((1 << b_mask) - 1)) << b_pos;
        return (unsigned short)(r_val | g_val | b_val);
    }
    // Default RGB 5:6:5 fallback (or 5:5:5 for 15bpp)
    if (screen_bpp == 15) {
        return (unsigned short)((((r & 0xF8) >> 3) << 10) | (((g & 0xF8) >> 3) << 5) | ((b & 0xF8) >> 3));
    }
    return RGB16(r, g, b);
}

static void set_vga_palette_full(const struct vbe_mode_info* info, int is_direct_color)
{
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        
        if (i < 16) {
            r = vga16[i][0];
            g = vga16[i][1];
            b = vga16[i][2];
        } else if (i < 32) {
            int level = (i - 16) * 4;
            r = g = b = level;
        } else {
            int n = i - 32;
            r = (n % 6) * 12;
            g = ((n / 6) % 6) * 12;
            b = ((n / 36) % 6) * 12;
        }
        
        unsigned short c16 = make_color16(r * 4, g * 4, b * 4, info);
        if (screen_bpp == 15) c16 &= 0x7FFF;
        palette16[i] = c16;
        palette32[i] = ((unsigned int)(r * 4) << 16) | ((unsigned int)(g * 4) << 8) | (unsigned int)(b * 4);

        if (!is_direct_color) {
            if (i == 0) outb(0x3C8, 0);
            outb(0x3C9, r);
            outb(0x3C9, g);
            outb(0x3C9, b);
        }
    }
}


void copy_rect_to_lfb(const unsigned char* src, int rx, int ry, int rw, int rh)
{
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > screen_w) rw = screen_w - rx;
    if (ry + rh > screen_h) rh = screen_h - ry;
    if (rw <= 0 || rh <= 0) return;

    int bytes_per_pixel = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
    int line_bytes = rw * bytes_per_pixel;
    
    for (int y = 0; y < rh; y++) {
        int offset = (ry + y) * screen_pitch + rx * bytes_per_pixel;
        const unsigned char* s = src + offset;
        unsigned char* d = (unsigned char*)lfb_ptr + offset;
        
        int dwords = line_bytes / 4;
        for (int i = 0; i < dwords; i++) {
            ((unsigned int*)d)[i] = ((const unsigned int*)s)[i];
        }
        for (int i = dwords * 4; i < line_bytes; i++) {
            d[i] = s[i];
        }
    }
    gfx_accel_present_rect(rx, ry, rw, rh);
}

void copy_fullscreen_to_lfb(const unsigned char* s_buf)
{
    int bytes_per_pixel = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
    int bytes_per_line = screen_w * bytes_per_pixel;
    if (bytes_per_line == screen_pitch) {
        int dwords = (bytes_per_line * screen_h) / 4;
        const unsigned int* s = (const unsigned int*)s_buf;
        unsigned int* d = (unsigned int*)lfb_ptr;
        for (int i = 0; i < dwords; i++) {
            d[i] = s[i];
        }
        for (int i = dwords * 4; i < bytes_per_line * screen_h; i++) {
            ((unsigned char*)d)[i] = s_buf[i];
        }
    } else {
        for (int y = 0; y < screen_h; y++) {
            const unsigned char* s = s_buf + y * screen_pitch;
            unsigned char* d = (unsigned char*)lfb_ptr + y * screen_pitch;
            int dwords = bytes_per_line / 4;
            for (int i = 0; i < dwords; i++) {
                ((unsigned int*)d)[i] = ((const unsigned int*)s)[i];
            }
            for (int i = dwords * 4; i < bytes_per_line; i++) {
                d[i] = s[i];
            }
        }
    }
    gfx_accel_present_rect(0, 0, screen_w, screen_h);
}

static int try_set_vbe_mode(int mode, int req_w, int req_h, int req_bpp)
{
    // Clear the 256-byte VBE buffer
    for (int i = 0; i < 256; i++) ((unsigned char*)0x9000)[i] = 0;
    
    struct vbe_mode_info* info = (struct vbe_mode_info*)0x9000;
    int ret_info = vbe_get_mode_info(mode);
    if (ret_info != 0x004F) return 0;
    if (!(info->attributes & 0x01)) return 0; // Mode not supported in hardware
    
    unsigned int found_lfb = 0;
    
    // Attempt 1: VBE 2.0+ LFB mode (mode | 0x4000)
    int ret_set = vbe_set_mode(mode | 0x4000);
    if (ret_set == 0x004F && info->phys_base_ptr != 0) {
        found_lfb = info->phys_base_ptr;
    } else {
        // Attempt 2: VBE 1.2 banked mode (without 0x4000) + PCI LFB fallback
        ret_set = vbe_set_mode(mode);
        if (ret_set == 0x004F) {
            if (pci_vga && pci_vga->bar_valid[0] &&
                !pci_vga->bar_is_io[0] && pci_vga->bar[0] != 0) {
                if (pci_vga->vendor == 0x5333) {
                    s3_enable_law();
                }
                found_lfb = pci_vga->bar[0];
            }
        }
    }
    
    if (found_lfb == 0) return 0;
    
    idt_reload();
    pic_remap();
    asm volatile("cli");
    
    lfb_ptr = found_lfb;
    lfb = (unsigned char*)found_lfb;
    screen_w = (info->x_res > 0) ? info->x_res : req_w;
    screen_h = (info->y_res > 0) ? info->y_res : req_h;

    if (req_bpp == 32) {
        if (info->bpp == 32 && info->bytes_per_scanline >= screen_w * 4) {
            screen_bpp = 32;
            screen_pitch = info->bytes_per_scanline;
        } else {
            return 0; // Hardware mode is not 32-bit! Prevent scanline skew corruption.
        }
    } else if (req_bpp == 24) {
        screen_bpp = 24;
        screen_pitch = (info->bytes_per_scanline >= screen_w * 3) ? info->bytes_per_scanline : (screen_w * 3);
    } else if (req_bpp == 16 || req_bpp == 15) {
        screen_bpp = (info->bpp == 15) ? 15 : 16;
        screen_pitch = (info->bytes_per_scanline >= screen_w * 2) ? info->bytes_per_scanline : (screen_w * 2);
    } else if (req_bpp <= 8) {
        screen_bpp = 8;
        screen_pitch = (info->bytes_per_scanline >= screen_w) ? info->bytes_per_scanline : screen_w;
    } else {
        screen_bpp = (info->bpp > 0) ? info->bpp : 16;
        int bpp_bytes = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
        screen_pitch = (info->bytes_per_scanline > 0) ? info->bytes_per_scanline : (screen_w * bpp_bytes);
    }
    
    set_vga_palette_full(info, (screen_bpp > 8));
    extern void gfx_accel_set_mode(int w, int h, int bpp);
    gfx_accel_set_mode(screen_w, screen_h, screen_bpp);
    return 1;
}

static int try_set_mach64_32_mode(int mode24, int req_w, int req_h)
{
    if (!ati_mach64_supports_native_32bpp(req_w, req_h)) return 0;
    if (!try_set_vbe_mode(mode24, req_w, req_h, 24)) return 0;
    if (!ati_mach64_enable_native_32bpp(screen_w, screen_h)) return 0;

    screen_bpp = 32;
    screen_pitch = (screen_w * 4 + 7) & ~7;
    set_vga_palette_full(0, 1);
    extern void gfx_accel_set_mode(int w, int h, int bpp);
    gfx_accel_set_mode(screen_w, screen_h, 32);
    return 1;
}

int vbe_find_mode(int req_w, int req_h, int req_bpp)
{
    if (vbe_get_controller_info() == 0x004F) {
        unsigned char* vbe_block = (unsigned char*)0x8800;
        if (vbe_block[0] == 'V' && vbe_block[1] == 'E' && vbe_block[2] == 'S' && vbe_block[3] == 'A') {
            unsigned short mode_off = *(unsigned short*)(vbe_block + 14);
            unsigned short mode_seg = *(unsigned short*)(vbe_block + 16);
            unsigned short* mode_list = (unsigned short*)((mode_seg << 4) + mode_off);
            for (int i = 0; mode_list[i] != 0xFFFF; i++) {
                int mode = mode_list[i] & 0x1FF;
                for (int c = 0; c < 256; c++) ((unsigned char*)0x9000)[c] = 0;
                struct vbe_mode_info* minfo = (struct vbe_mode_info*)0x9000;
                if (vbe_get_mode_info(mode) == 0x004F) {
                    if (minfo->x_res == req_w && minfo->y_res == req_h && (minfo->attributes & 0x01)) {
                        if (req_bpp == 32) {
                            if (minfo->bpp == 32 &&
                                minfo->bytes_per_scanline >= req_w * 4) {
                                return mode;
                            }
                        } else if (req_bpp == 24) {
                            if (minfo->bpp == 24 && minfo->bytes_per_scanline < req_w * 4) {
                                return mode;
                            }
                        } else if (req_bpp == 16 || req_bpp == 15) {
                            if (minfo->bpp == 16 || minfo->bpp == 15) {
                                return mode;
                            }
                        } else if (req_bpp <= 8) {
                            if (minfo->bpp <= 8) {
                                return mode;
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}

int vbe_supports_mode(int w, int h, int bpp)
{
    if (w <= 0 || h <= 0 || bpp <= 0) return 0;
    if (w == 320 && h == 200 && bpp == 8) return 1;
    extern int gfx_accel_supports_bpp(int w, int h, int bpp);
    return gfx_accel_supports_bpp(w, h, bpp);
}

static void set_resolution_locked(int w, int h, int bpp)
{
    // Bounds check to ensure resolution fits within backbuffer
    if (w <= 0 || h <= 0 || w > 1024 || h > 768) return;
    int bpp_bytes = (bpp <= 8) ? 1 : ((bpp <= 16) ? 2 : ((bpp <= 24) ? 3 : 4));
    if ((unsigned int)(w * h * bpp_bytes) > sizeof(backbuffer)) return;

    if (gfx_accel_try_set_mode(w, h, bpp)) {
        set_vga_palette_full(0, 1);
        return;
    }

    if (w == 320 && h == 200 && bpp == 8) {
        asm volatile("cli");
        bios_set_mode(0x13);
        idt_reload();
        pic_remap();
        asm volatile("cli");

        screen_w = 320; screen_h = 200; screen_bpp = 8; screen_pitch = 320;
        lfb_ptr = 0xA0000;
        lfb = (unsigned char*)0xA0000;
        
        // Restore DAC palette for 8-bit mode
        set_vga_palette_full(0, 0);
        extern void gfx_accel_set_mode(int w, int h, int bpp);
        gfx_accel_set_mode(320, 200, 8);
        return;
    }
    
    asm volatile("cli");
    
    // Ensure PCI devices are scanned
    if (!pci_vga) {
        pci_scan_vga();
    }
    
    int candidates[8] = {0};
    int num_candidates = 0;
    
    if (bpp == 32) {
        int m = vbe_find_mode(w, h, 32);
        if (m > 0 && try_set_vbe_mode(m, w, h, 32)) return;
        int m24 = vbe_find_mode(w, h, 24);
        if (m24 > 0 && try_set_mach64_32_mode(m24, w, h)) return;
        if (w == 800 && h == 600) {
            candidates[0] = 0x122; // ATI Mach64 800x600 32bpp
            candidates[1] = 0x143; // Bochs/SeaBIOS 800x600 32bpp
            candidates[2] = 0x138; // Standard OEM 32bpp
            num_candidates = 3;
        } else if (w == 640 && h == 480) {
            candidates[0] = 0x121; // ATI Mach64 640x480 32bpp
            candidates[1] = 0x140; // Bochs/SeaBIOS 640x480 32bpp
            candidates[2] = 0x10F;
            num_candidates = 3;
        } else if (w == 1024 && h == 768) {
            candidates[0] = 0x124; // ATI Mach64 1024x768 32bpp
            candidates[1] = 0x145; // Bochs/SeaBIOS 1024x768 32bpp
            candidates[2] = 0x139;
            num_candidates = 3;
        }
    } else if (bpp == 24) {
        int m = vbe_find_mode(w, h, 24);
        if (m > 0 && try_set_vbe_mode(m, w, h, 24)) return;
        if (w == 800 && h == 600) {
            candidates[0] = 0x115; // 800x600 24bpp
            num_candidates = 1;
        } else if (w == 640 && h == 480) {
            candidates[0] = 0x112; // 640x480 24bpp
            num_candidates = 1;
        } else if (w == 1024 && h == 768) {
            candidates[0] = 0x118; // 1024x768 24bpp
            num_candidates = 1;
        }
    } else if (bpp <= 8) {
        if (w == 800 && h == 600) {
            candidates[0] = 0x103; // 800x600 8bpp
            num_candidates = 1;
        } else if (w == 640 && h == 480) {
            candidates[0] = 0x101; // 640x480 8bpp
            num_candidates = 1;
        } else if (w == 1024 && h == 768) {
            candidates[0] = 0x105; // 1024x768 8bpp
            num_candidates = 1;
        }
    } else { // 15 or 16 bpp
        if (w == 800 && h == 600) {
            candidates[0] = 0x114; // 800x600 16bpp (64K colors)
            candidates[1] = 0x113; // 800x600 15bpp (32K colors)
            num_candidates = 2;
        } else if (w == 640 && h == 480) {
            candidates[0] = 0x111; // 640x480 16bpp (64K colors)
            candidates[1] = 0x110; // 640x480 15bpp (32K colors)
            num_candidates = 2;
        } else if (w == 1024 && h == 768) {
            candidates[0] = 0x117; // 1024x768 16bpp (64K colors)
            candidates[1] = 0x116; // 1024x768 15bpp (32K colors)
            num_candidates = 2;
        }
    }
    
    for (int i = 0; i < num_candidates; i++) {
        if (try_set_vbe_mode(candidates[i], w, h, bpp)) {
            return;
        }
    }


    // Fallback: If requested mode failed, try 800x600 16bpp then 640x480 16bpp
    if (try_set_vbe_mode(0x114, 800, 600, 16)) return;
    if (try_set_vbe_mode(0x111, 640, 480, 16)) return;
    if (try_set_vbe_mode(0x110, 640, 480, 15)) return;

    // Fallback: Standard VGA 320x200 8-bit mode 0x13
    bios_set_mode(0x13);
    idt_reload();
    pic_remap();
    asm volatile("cli");
    screen_w = 320; screen_h = 200; screen_bpp = 8; screen_pitch = 320;
    lfb_ptr = 0xA0000;
    lfb = (unsigned char*)0xA0000;
    set_vga_palette_full(0, 0);
    extern void gfx_accel_set_mode(int w, int h, int bpp);
    gfx_accel_set_mode(320, 200, 8);
}

void set_resolution(int w, int h, int bpp)
{
    irq_bios_state_t irq_state;
    irq_bios_suspend(&irq_state);
    set_resolution_locked(w, h, bpp);
    idt_reload();
    pic_remap();
    irq_bios_resume(&irq_state);
}

static void timer_irq_dispatch(unsigned int irq, void* context)
{
    (void)irq;
    (void)context;
    timer_handle_irq();
}

static void ps2_poll_input(void)
{
    unsigned int flags = irq_save_flags();
    int drained = 0;
    while ((inb(0x64) & 0x01) && drained++ < 32) {
        unsigned char status = inb(0x64);
        unsigned char data = inb(0x60);
        if (status & 0x20) mouse_handle_byte(data);
        else keyboard_handle_byte(data);
    }
    irq_restore_flags(flags);
}

static void ps2_irq_dispatch(unsigned int irq, void* context)
{
    (void)irq;
    (void)context;
    ps2_poll_input();
}

void kernel_main(void)
{
    log_init();
    log_printf("[BOOT] Pixel Neko Next %s P0/P1 bring-up\n", KERNEL_BUILD);

    // Ensure CPU L1/L2 Cache is active and x87 FPU is enabled
    asm volatile (
        "movl %%cr0, %%eax\n\t"
        "andl $0x9FFFFFF3, %%eax\n\t" // Clear CD (30), NW (29), EM (2), TS (3)
        "orl  $0x02, %%eax\n\t"       // Set MP (1)
        "movl %%eax, %%cr0\n\t"
        "wbinvd\n\t"
        "fninit\n\t"                  // Initialize x87 FPU
        : : : "eax"
    );

    idt_init();
    irq_init();
    timer_init();
    irq_register_handler(0, timer_irq_dispatch, 0);
    cpu_detect();
    log_printf("[CPU ] %s at %uMHz\n", cpu_model_string,
               g_cpu_info.mhz > 0 ? g_cpu_info.mhz : 200);
    pmm_init();
    log_printf("[MM  ] usable=%uMB free=%uMB\n",
               usable_ram_mb, pmm_get_free_mb());
    pci_scan_all();
    pci_scan_vga();
    speaker_init();
    audio_init();
    log_printf("[AUD ] %s\n", audio_get_hardware_name());
    set_resolution(800, 600, 32);
    enable_fpu(); // Re-enable x87 FPU / SSE after real-mode VBE switch
    timer_init(); // Re-verify PIT timer after real-mode VBE switch
    
    // Play Mac OS 9 & Windows 3.1 retro boot splash animation
    splash_run();

    usb_init();

    unsigned char* v = backbuffer;
    int mx, my, mb, last_mx = -1, last_my = -1, last_mb = 0;

    settings_init();
    settings_load();

    wm_init();
    wm_draw(v);
    copy_fullscreen_to_lfb(v);
    mouse_init();

    irq_register_handler(1, ps2_irq_dispatch, 0);
    irq_register_handler(12, ps2_irq_dispatch, 0);
    timer_set_irq_mode(1);
    irq_unmask(0);
    irq_unmask(1);
    irq_unmask(12);
    irq_global_enable();
    log_printf("[IRQ ] active timer/keyboard/mouse, IF=%u\n",
               (unsigned int)irq_interrupts_enabled());

    mouse_get(&mx, &my, &mb);
    int hardware_cursor = gfx_accel_has_hardware_cursor();
    if (hardware_cursor) gfx_accel_set_cursor(mx, my, 1);

    while (1) {
        usb_poll();
        audio_poll();
        timer_update();
        check_revert_timer();
        hardware_cursor = gfx_accel_has_hardware_cursor();

        ps2_poll_input();
        mouse_get(&mx, &my, &mb);
        int mb_press = (mb & 1) && !(last_mb & 1);
        int mb_release = !(mb & 1) && (last_mb & 1);
        last_mb = mb;

        int mr = wm_need_mouse_reset();
        int moved = (mx != last_mx || my != last_my);
        
        if (!hardware_cursor && (mr || moved)) {
            if (last_mx >= 0) {
                mouse_reset(v);
                copy_rect_to_lfb(v, last_mx, last_my, 16, 16);
            }
        }

        wm_update(v, mx, my, mb, mb_press, mb_release);
        int new_mr = wm_need_mouse_reset();
        if (new_mr) {
            mr = 1;
        }
        
        extern int redraw;
        int need_full_copy = redraw || (last_mx == -1);
        
        wm_draw(v);

        if (hardware_cursor) {
            if (need_full_copy) {
                copy_fullscreen_to_lfb(v);
                last_mx = mx; last_my = my;
            }
            gfx_accel_set_cursor(mx, my, 1);
        } else if (need_full_copy) {
            extern void mouse_invalidate(void);
            mouse_invalidate();
            mouse_draw(v);
            copy_fullscreen_to_lfb(v);
            last_mx = mx; last_my = my;
        } else if (mr || moved) {
            mouse_draw(v);
            copy_rect_to_lfb(v, mx, my, 16, 16);
            last_mx = mx; last_my = my;
        }
    }
}
