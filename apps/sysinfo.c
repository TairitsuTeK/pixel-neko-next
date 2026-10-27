#include "sysinfo.h"
#include "font.h"
#include "wm.h"
#include "cpu.h"
#include "mm.h"
#include "port.h"
#include "gfx_accel.h"
#include "vfs.h"
#include "audio.h"
#include "version.h"
#include "pci.h"
#include "keyboard.h"
#include "mouse.h"
#include "usb_hid.h"

static int active_tab = 0; // 0: System, 1: PCI Bus, 2: Drives
static int pci_page = 0;

static const char* get_vendor_name(unsigned short vendor) {
    switch (vendor) {
        case 0x8086: return "Intel";
        case 0x1002: return "ATI Tech";
        case 0x5333: return "S3 Graphics";
        case 0x1022: return "AMD";
        case 0x1234: return "QEMU/Bochs";
        case 0x10EC: return "Realtek";
        case 0x1274: return "Ensoniq";
        case 0x1106: return "VIA Tech";
        case 0x10B7: return "3Com";
        case 0x1013: return "Cirrus";
        case 0x121A: return "3dfx";
        case 0x10DE: return "NVIDIA";
        case 0x13F6: return "C-Media";
        case 0x15AD: return "VMware";
        default:     return "Generic";
    }
}

static const char* get_class_name(unsigned char base_class) {
    switch (base_class) {
        case 0x01: return "Storage/IDE";
        case 0x02: return "Network";
        case 0x03: return "VGA Display";
        case 0x04: return "Multimedia";
        case 0x06: return "Bridge/Host";
        case 0x0C: return "Serial/USB";
        default:   return "Other";
    }
}

static void scan_pci_bus(void) {
    pci_scan_all();
}

void sysinfo_init(void) {
    active_tab = 0;
    pci_page = 0;
    scan_pci_bus();
}

static void int_to_str(unsigned int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int ti = 0, bi = 0;
    while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
    while (ti > 0) buf[bi++] = tmp[--ti];
    buf[bi] = '\0';
}

static void draw_text_fitted(unsigned char* v, int x, int y, int max_chars,
                             const char* text, unsigned char color) {
    char out[80];
    int len = 0;
    if (max_chars < 4) max_chars = 4;
    if (max_chars > 76) max_chars = 76;
    while (text[len]) len++;

    if (len <= max_chars) {
        for (int i = 0; i < len; i++) out[i] = text[i];
        out[len] = '\0';
    } else {
        int keep = max_chars - 3;
        for (int i = 0; i < keep; i++) out[i] = text[i];
        out[keep] = '.';
        out[keep + 1] = '.';
        out[keep + 2] = '.';
        out[max_chars] = '\0';
    }
    font_draw_text(v, x, y, out, color);
}

static void append_input_name(char* out, int cap, int* pos, const char* name) {
    if (*pos > 0 && *pos + 3 < cap) {
        out[(*pos)++] = ' ';
        out[(*pos)++] = '+';
        out[(*pos)++] = ' ';
    }
    while (*name && *pos < cap - 1) out[(*pos)++] = *name++;
    out[*pos] = '\0';
}

static int build_input_summary(char* out, int cap) {
    int pos = 0;
    out[0] = '\0';

    if (keyboard_is_ps2_active()) append_input_name(out, cap, &pos, "PS/2 kbd");
    if (usb_hid_keyboard_count() > 0) append_input_name(out, cap, &pos, "USB kbd");
    if (mouse_is_ps2_active()) append_input_name(out, cap, &pos, "PS/2 mouse");
    if (usb_hid_mouse_count() > 0) append_input_name(out, cap, &pos, "USB mouse");

    if (pos == 0) {
        const char* none = "None detected";
        while (*none && pos < cap - 1) out[pos++] = *none++;
        out[pos] = '\0';
    }
    return pos;
}

static int draw_text_wrapped(unsigned char* v, int x, int y, int max_chars,
                             int max_lines, const char* text,
                             unsigned char color) {
    char line[80];
    int pos = 0;
    int line_no = 0;

    if (max_chars < 4) max_chars = 4;
    if (max_chars > 76) max_chars = 76;

    while (text[pos] && line_no < max_lines) {
        while (text[pos] == ' ') pos++;
        if (!text[pos]) break;

        int start = pos;
        int count = 0;
        int last_space = -1;
        while (text[pos] && count < max_chars) {
            if (text[pos] == ' ') last_space = pos;
            pos++;
            count++;
        }
        if (text[pos] && last_space > start) pos = last_space;

        int end = pos;
        while (end > start && text[end - 1] == ' ') end--;
        int len = end - start;
        if (len > max_chars) len = max_chars;
        for (int i = 0; i < len; i++) line[i] = text[start + i];

        if (line_no == max_lines - 1 && text[pos]) {
            if (len > max_chars - 3) len = max_chars - 3;
            line[len++] = '.';
            line[len++] = '.';
            line[len++] = '.';
        }
        line[len] = '\0';
        font_draw_text(v, x, y, line, color);

        while (text[pos] == ' ') pos++;
        line_no++;
        y += 16;
    }

    return line_no > 0 ? line_no : 1;
}

static void hex16_to_str(unsigned short val, char* out) {
    const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(val >> 12) & 0xF];
    out[1] = hex[(val >> 8) & 0xF];
    out[2] = hex[(val >> 4) & 0xF];
    out[3] = hex[val & 0xF];
    out[4] = '\0';
}

static void format_kb_str(unsigned int kb, char* out) {
    if (kb == 0) {
        out[0] = '0'; out[1] = ' '; out[2] = 'K'; out[3] = 'B'; out[4] = '\0';
        return;
    }
    if (kb >= 1024 * 1024) {
        unsigned int gb_whole = kb / (1024 * 1024);
        unsigned int gb_dec = ((kb % (1024 * 1024)) * 100) / (1024 * 1024);
        char buf[12]; int_to_str(gb_whole, buf);
        int i = 0; while (buf[i]) { out[i] = buf[i]; i++; }
        out[i++] = '.';
        out[i++] = '0' + (gb_dec / 10);
        out[i++] = '0' + (gb_dec % 10);
        out[i++] = ' '; out[i++] = 'G'; out[i++] = 'B'; out[i++] = '\0';
    } else if (kb >= 1024) {
        unsigned int mb_whole = kb / 1024;
        unsigned int mb_dec = ((kb % 1024) * 10) / 1024;
        char buf[12]; int_to_str(mb_whole, buf);
        int i = 0; while (buf[i]) { out[i] = buf[i]; i++; }
        out[i++] = '.';
        out[i++] = '0' + mb_dec;
        out[i++] = ' '; out[i++] = 'M'; out[i++] = 'B'; out[i++] = '\0';
    } else {
        char buf[12]; int_to_str(kb, buf);
        int i = 0; while (buf[i]) { out[i] = buf[i]; i++; }
        out[i++] = ' '; out[i++] = 'K'; out[i++] = 'B'; out[i++] = '\0';
    }
}

void sysinfo_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    // Window Body
    for (int y = wy + 20; y < wy + wh - 1; y++) {
        for (int x = wx + 1; x < wx + ww - 1; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }

    // Tabs Header
    const char* tab_names[3] = { "System", "PCI Bus", "Drives" };
    int tab_w = 95, tab_h = 18;
    for (int t = 0; t < 3; t++) {
        int tx = wx + 10 + t * 100;
        int ty = wy + 24;
        int active = (t == active_tab);

        for (int y = ty; y < ty + tab_h; y++) {
            for (int x = tx; x < tx + tab_w; x++) {
                PUT_PIXEL(v, x, y, active ? 15 : 8);
            }
        }
        for (int x = tx; x < tx + tab_w; x++) {
            PUT_PIXEL(v, x, ty, active ? 15 : 7);
            PUT_PIXEL(v, x, ty + tab_h - 1, 0);
        }
        for (int y = ty; y < ty + tab_h; y++) {
            PUT_PIXEL(v, tx, y, active ? 15 : 7);
            PUT_PIXEL(v, tx + tab_w - 1, y, 0);
        }
        int tw = 0; while (tab_names[t][tw]) tw++;
        int text_x = tx + (tab_w - tw * 8) / 2;
        font_draw_text(v, text_x, ty + 3, tab_names[t], active ? 0 : 15);
    }

    // Inner Content Frame
    int fx = wx + 10, fy = wy + 46;
    int fw = ww - 20, fh = wh - 54;
    for (int x = fx; x < fx + fw; x++) {
        PUT_PIXEL(v, x, fy, 0);
        PUT_PIXEL(v, x, fy + fh - 1, 15);
    }
    for (int y = fy; y < fy + fh; y++) {
        PUT_PIXEL(v, fx, y, 0);
        PUT_PIXEL(v, fx + fw - 1, y, 15);
    }

    if (active_tab == 0) {
        // Tab 0: System
        int lx = fx + 10, ly = fy + 8;
        int value_x = lx + 125;
        int max_value_chars = (fx + fw - 10 - value_x) / 8;
        if (max_value_chars < 4) max_value_chars = 4;

        font_draw_text(v, lx, ly, "OS Version:", 0);
        draw_text_fitted(v, value_x, ly, max_value_chars,
                         "Pixel Neko Next (Build " KERNEL_BUILD ")", 1);

        ly += 16;
        font_draw_text(v, lx, ly, "Processor:", 0);
        const char* brand = cpu_model_string[0] ? cpu_model_string : "i686 Compatible CPU";
        draw_text_fitted(v, value_x, ly, max_value_chars, brand, 0);

        ly += 16;
        font_draw_text(v, lx, ly, "Frequency:", 0);
        char mhz_buf[16];
        int_to_str(g_cpu_info.mhz > 0 ? g_cpu_info.mhz : 200, mhz_buf);
        int mi = 0; while (mhz_buf[mi]) mi++;
        mhz_buf[mi++] = ' '; mhz_buf[mi++] = 'M'; mhz_buf[mi++] = 'H'; mhz_buf[mi++] = 'z'; mhz_buf[mi] = '\0';
        draw_text_fitted(v, value_x, ly, max_value_chars, mhz_buf, 0);

        ly += 16;
        font_draw_text(v, lx, ly, "Features:", 0);
        draw_text_fitted(v, value_x, ly, max_value_chars,
                         "FPU, TSC, MSR, PAE, APIC, CMOV", 8);

        ly += 16;
        font_draw_text(v, lx, ly, "Installed RAM:", 0);
        char ram_buf[24];
        char num1[12], num2[12];
        int_to_str(usable_ram_mb, num1);
        int_to_str(total_ram_mb, num2);
        int ri = 0;
        for (int i = 0; num1[i]; i++) ram_buf[ri++] = num1[i];
        ram_buf[ri++] = ' '; ram_buf[ri++] = 'M'; ram_buf[ri++] = 'B';
        ram_buf[ri++] = ' '; ram_buf[ri++] = '(';
        for (int i = 0; num2[i]; i++) ram_buf[ri++] = num2[i];
        ram_buf[ri++] = ' '; ram_buf[ri++] = 'M'; ram_buf[ri++] = 'B'; ram_buf[ri++] = ' ';
        ram_buf[ri++] = 'T'; ram_buf[ri++] = 'o'; ram_buf[ri++] = 't'; ram_buf[ri++] = 'a'; ram_buf[ri++] = 'l'; ram_buf[ri++] = ')';
        ram_buf[ri] = '\0';
        draw_text_fitted(v, value_x, ly, max_value_chars, ram_buf, 0);

        ly += 16;
        font_draw_text(v, lx, ly, "Graphics Card:", 0);
        draw_text_fitted(v, value_x, ly, max_value_chars, gfx_accel_get_name(), 0);

        ly += 16;
        font_draw_text(v, lx, ly, "Video Memory:", 0);
        char vram_buf[16];
        int vram_mb = gfx_accel_get_vram_size() / (1024 * 1024);
        int_to_str(vram_mb, vram_buf);
        int vi = 0; while (vram_buf[vi]) vi++;
        vram_buf[vi++] = ' '; vram_buf[vi++] = 'M'; vram_buf[vi++] = 'B'; vram_buf[vi] = '\0';
        draw_text_fitted(v, value_x, ly, max_value_chars, vram_buf, 0);

        ly += 16;
        font_draw_text(v, lx, ly, "2D Engine:", 0);
        if (gfx_accel_is_active()) {
            draw_text_fitted(v, value_x, ly, max_value_chars,
                             "Hardware 2D (Active)", 2);
        } else {
            draw_text_fitted(v, value_x, ly, max_value_chars,
                             "Software Rasterizer (Fallback)", 8);
        }

        ly += 16;
        font_draw_text(v, lx, ly, "Driver Status:", 0);
        draw_text_fitted(v, value_x, ly, max_value_chars,
                         gfx_accel_get_status(), 8);

        ly += 16;
        font_draw_text(v, lx, ly, "Audio Device:", 0);
        int audio_lines = 1;
        if (audio_has_hardware()) {
            audio_lines = draw_text_wrapped(v, value_x, ly, max_value_chars, 2,
                                            audio_get_hardware_name(), 0);
        } else {
            audio_lines = draw_text_wrapped(v, value_x, ly, max_value_chars, 2,
                                            audio_get_probe_status(), 8);
        }
        ly += (audio_lines - 1) * 16;

        ly += 16;
        font_draw_text(v, lx, ly, "Audio Engine:", 0);
        if (audio_has_hardware()) {
            draw_text_fitted(v, value_x, ly, max_value_chars,
                             "Kernel DSP Engine (Active)", 2);
        } else {
            draw_text_fitted(v, value_x, ly, max_value_chars, "Disabled", 8);
        }

        ly += 16;
        int input_y = ly;
        int input_bottom = fy + fh - 10;
        if (input_y > input_bottom) input_y = input_bottom;
        font_draw_text(v, lx, input_y, "Input Devices:", 0);
        char input_summary[96];
        build_input_summary(input_summary, sizeof(input_summary));
        int input_active = keyboard_is_ps2_active() || mouse_is_ps2_active() ||
                           usb_hid_keyboard_count() > 0 ||
                           usb_hid_mouse_count() > 0;
        draw_text_fitted(v, value_x, input_y, max_value_chars, input_summary,
                         input_active ? 2 : 8);

        int ctrl_y = input_y + 16;
        int ctrl_bottom = fy + fh - 10;
        if (ctrl_y <= ctrl_bottom) {
            font_draw_text(v, lx, ctrl_y, "USB Controller:", 0);
            draw_text_fitted(v, value_x, ctrl_y, max_value_chars,
                             usb_hid_controller_summary(),
                             usb_hid_controller_active() ? 2 : 8);
        }

    } else if (active_tab == 1) {
        // Tab 1: PCI Devices (6 items per page)
        scan_pci_bus();
        int pci_count = pci_get_device_count();
        int lx = fx + 8, ly = fy + 6;
        font_draw_text(v, lx, ly, "PCI Devices (Bus 0..255):", 0);

        int start_idx = pci_page * 6;
        int max_pages = (pci_count + 5) / 6;
        if (max_pages == 0) max_pages = 1;

        ly += 14;
        for (int i = start_idx; i < start_idx + 6 && i < pci_count; i++) {
            const pci_device_t* pci = pci_get_device(i);
            if (!pci) continue;
            char line[64];
            const char* hex = "0123456789ABCDEF";
            line[0] = hex[(pci->bus >> 4) & 0xF];
            line[1] = hex[pci->bus & 0xF];
            line[2] = ':';
            line[3] = '0' + (pci->slot / 10);
            line[4] = '0' + (pci->slot % 10);
            line[5] = '.';
            line[6] = '0' + pci->function;
            line[7] = ' '; line[8] = '[';
            hex16_to_str(pci->vendor, &line[9]);
            line[13] = ':';
            hex16_to_str(pci->device, &line[14]);
            line[18] = ']'; line[19] = ' ';
            const char* vname = get_vendor_name(pci->vendor);
            int li = 20;
            while (*vname && li < 32) line[li++] = *vname++;
            while (li < 33) line[li++] = ' ';
            const char* cname = get_class_name(pci->class_code);
            while (*cname && li < 50) line[li++] = *cname++;
            line[li] = '\0';

            font_draw_text(v, lx, ly, line, 0);
            ly += 14;
        }

        // Pagination buttons
        int pbtn_y = fy + fh - 20;
        int prev_x = fx + 14;
        int next_x = fx + fw - 84;
        font_draw_text(v, prev_x, pbtn_y, "[ < Prev ]", pci_page > 0 ? 0 : 8);
        font_draw_text(v, next_x, pbtn_y, "[ Next > ]", pci_page < max_pages - 1 ? 0 : 8);

    } else if (active_tab == 2) {
        // Tab 2: Storage Drives (Multi-column aligned)
        int lx = fx + 8, ly = fy + 6;
        font_draw_text(v, lx, ly, "Drive", 0);
        font_draw_text(v, lx + 56, ly, "FileSystem", 0);
        font_draw_text(v, lx + 165, ly, "Total Size", 0);
        font_draw_text(v, lx + 275, ly, "Free Space", 0);
        ly += 16;

        for (int letter = 'A'; letter <= 'Z'; letter++) {
            vfs_drive_t* d = vfs_get_drive((char)letter);
            if (d && d->present) {
                char drv_buf[8] = { '[', (char)letter, ':', ']', 0 };
                const char* fs = (d->drv_type == VFS_DRV_FLOPPY) ? "FAT12" :
                                 ((d->drv_type == VFS_DRV_CDROM) ? "CDFS" : vfs_get_fs_name(d->fs_type));

                unsigned int tot_k = 0, fre_k = 0;
                int is_known = 0;
                vfs_get_drive_space((char)letter, &tot_k, &fre_k, &is_known);

                char tot_buf[16], fre_buf[16];
                format_kb_str(tot_k, tot_buf);
                if (is_known) {
                    format_kb_str(fre_k, fre_buf);
                } else if (d->is_readonly) {
                    fre_buf[0] = 'R'; fre_buf[1] = 'e'; fre_buf[2] = 'a'; fre_buf[3] = 'd';
                    fre_buf[4] = 'O'; fre_buf[5] = 'n'; fre_buf[6] = 'l'; fre_buf[7] = 'y'; fre_buf[8] = '\0';
                } else {
                    fre_buf[0] = '-'; fre_buf[1] = '-'; fre_buf[2] = '\0';
                }

                font_draw_text(v, lx, ly, drv_buf, 0);
                font_draw_text(v, lx + 56, ly, fs, 0);
                font_draw_text(v, lx + 165, ly, tot_buf, 0);
                font_draw_text(v, lx + 275, ly, fre_buf, 0);
                ly += 14;
            }
        }
    }
}

void sysinfo_click(int mx, int my, int wx, int wy, int ww, int wh) {
    // Check tab clicks
    int tab_w = 95, tab_h = 18;
    for (int t = 0; t < 3; t++) {
        int tx = wx + 10 + t * 100;
        int ty = wy + 24;
        if (mx >= tx && mx < tx + tab_w && my >= ty && my < ty + tab_h) {
            active_tab = t;
            return;
        }
    }

    // Check PCI Page clicks
    if (active_tab == 1) {
        int pci_count = pci_get_device_count();
        int fx = wx + 10, fy = wy + 46;
        int fw = ww - 20, fh = wh - 54;
        int pbtn_y = fy + fh - 20;
        int prev_x = fx + 14;
        int next_x = fx + fw - 84;

        if (my >= pbtn_y - 2 && my <= pbtn_y + 14) {
            if (mx >= prev_x && mx <= prev_x + 80 && pci_page > 0) {
                pci_page--;
            } else if (mx >= next_x && mx <= next_x + 80) {
                int max_pages = (pci_count + 5) / 6;
                if (pci_page < max_pages - 1) {
                    pci_page++;
                }
            }
        }
    }
}
