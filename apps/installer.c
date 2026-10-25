#include "installer.h"
#include "wm.h"
#include "font.h"
#include "vfs.h"
#include "fat.h"
#include "blkdev.h"
#include "part.h"
#include "mm.h"
#include "power.h"
#include "grldr_boot_bins.h"

extern int redraw;
extern int insv;

typedef enum {
    STEP_SELECT = 0,
    STEP_OVERWRITE = 1,
    STEP_CONFIRM = 2,
    STEP_PROGRESS = 3,
    STEP_COMPLETE = 4
} install_step_t;

static install_step_t g_step = STEP_SELECT;
static int g_selected_idx = 0;
static char g_target_letter = 'C';
static int g_install_error = 0;
static char g_existing_desc[64] = "";
static int g_has_existing_files = 0;

typedef struct {
    char letter;
    char fs_name[16];
    unsigned int total_mb;
    unsigned int free_mb;
    int is_free_known;
    vfs_drive_t* drv;
} target_part_t;

static target_part_t g_parts[8];
static int g_part_count = 0;

static void my_itoa_inst(unsigned int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    buf[i] = '\0';
    int start = 0, end = i - 1;
    while (start < end) {
        char t = buf[start]; buf[start] = buf[end]; buf[end] = t;
        start++; end--;
    }
}

static void str_cat_inst(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static int g_progress_percent = 0;
static char g_progress_text[64] = "Preparing installation...";
static char g_progress_subtext[64] = "";

static void update_progress_ui(int percent, const char* text, const char* subtext) {
    g_progress_percent = percent;
    int k = 0; while (text && text[k] && k < 63) { g_progress_text[k] = text[k]; k++; }
    g_progress_text[k] = '\0';
    k = 0; while (subtext && subtext[k] && k < 63) { g_progress_subtext[k] = subtext[k]; k++; }
    g_progress_subtext[k] = '\0';

    extern unsigned char backbuffer[];
    extern void copy_fullscreen_to_lfb(const unsigned char* s_buf);
    wm_draw(backbuffer);
    copy_fullscreen_to_lfb(backbuffer);
}

static void on_kernel_write_progress(unsigned int written, unsigned int total) {
    if (total == 0) return;
    int pct = 30 + (int)((written * 45) / total);
    if (pct > 75) pct = 75;
    char num_buf[48];
    unsigned int wr_kb = written / 1024;
    unsigned int tot_kb = total / 1024;
    char w_str[16], t_str[16];
    my_itoa_inst(wr_kb, w_str);
    my_itoa_inst(tot_kb, t_str);
    num_buf[0] = '\0';
    str_cat_inst(num_buf, "Writing kernel: ");
    str_cat_inst(num_buf, w_str);
    str_cat_inst(num_buf, " / ");
    str_cat_inst(num_buf, t_str);
    str_cat_inst(num_buf, " KB");
    update_progress_ui(pct, "Copying system kernel...", num_buf);
}

static int check_existing_boot_files(char target_letter, char* desc, int max_len) {
    if (desc && max_len > 0) desc[0] = '\0';
    int found_count = 0;
    vfs_dirent_t de;
    char p[32];

    // Check \boot\kernel.elf or \kernel.elf
    p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'b'; p[4] = 'o'; p[5] = 'o'; p[6] = 't';
    p[7] = '\\'; p[8] = 'k'; p[9] = 'e'; p[10] = 'r'; p[11] = 'n'; p[12] = 'e'; p[13] = 'l';
    p[14] = '.'; p[15] = 'e'; p[16] = 'l'; p[17] = 'f'; p[18] = '\0';
    if (vfs_find_file(p, &de) == 0) {
        if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
        if (desc) str_cat_inst(desc, "kernel.elf");
    } else {
        p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'k'; p[4] = 'e'; p[5] = 'r';
        p[6] = 'n'; p[7] = 'e'; p[8] = 'l'; p[9] = '.'; p[10] = 'e'; p[11] = 'l'; p[12] = 'f'; p[13] = '\0';
        if (vfs_find_file(p, &de) == 0) {
            if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
            if (desc) str_cat_inst(desc, "kernel.elf");
        }
    }

    // Check \grldr or \boot\grldr
    p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'g'; p[4] = 'r'; p[5] = 'l';
    p[6] = 'd'; p[7] = 'r'; p[8] = '\0';
    if (vfs_find_file(p, &de) == 0) {
        if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
        if (desc) str_cat_inst(desc, "grldr");
    } else {
        p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'b'; p[4] = 'o'; p[5] = 'o';
        p[6] = 't'; p[7] = '\\'; p[8] = 'g'; p[9] = 'r'; p[10] = 'l'; p[11] = 'd'; p[12] = 'r'; p[13] = '\0';
        if (vfs_find_file(p, &de) == 0) {
            if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
            if (desc) str_cat_inst(desc, "grldr");
        }
    }

    // Check \menu.lst or \boot\menu.lst
    p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'm'; p[4] = 'e'; p[5] = 'n';
    p[6] = 'u'; p[7] = '.'; p[8] = 'l'; p[9] = 's'; p[10] = 't'; p[11] = '\0';
    if (vfs_find_file(p, &de) == 0) {
        if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
        if (desc) str_cat_inst(desc, "menu.lst");
    } else {
        p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'b'; p[4] = 'o'; p[5] = 'o';
        p[6] = 't'; p[7] = '\\'; p[8] = 'm'; p[9] = 'e'; p[10] = 'n'; p[11] = 'u'; p[12] = '.';
        p[13] = 'l'; p[14] = 's'; p[15] = 't'; p[16] = '\0';
        if (vfs_find_file(p, &de) == 0) {
            if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
            if (desc) str_cat_inst(desc, "menu.lst");
        }
    }

    // Fallback check for Windows bootmgr/ntldr
    if (found_count == 0) {
        p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'b'; p[4] = 'o'; p[5] = 'o';
        p[6] = 't'; p[7] = 'm'; p[8] = 'g'; p[9] = 'r'; p[10] = '\0';
        if (vfs_find_file(p, &de) == 0) {
            if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
            if (desc) str_cat_inst(desc, "bootmgr");
        } else {
            p[0] = target_letter; p[1] = ':'; p[2] = '\\'; p[3] = 'n'; p[4] = 't'; p[5] = 'l';
            p[6] = 'd'; p[7] = 'r'; p[8] = '\0';
            if (vfs_find_file(p, &de) == 0) {
                if (found_count++ > 0 && desc) str_cat_inst(desc, ", ");
                if (desc) str_cat_inst(desc, "ntldr");
            }
        }
    }

    return found_count;
}

static void scan_targets(void) {
    g_part_count = 0;
    for (int i = 0; i < 26; i++) {
        vfs_drive_t* d = vfs_get_drive('A' + i);
        if (d && d->present && d->drv_type == VFS_DRV_HDD && d->is_supported && d->dev) {
            if (d->fs_type == PART_FS_FAT12 || d->fs_type == PART_FS_FAT16 || d->fs_type == PART_FS_FAT32) {
                if (g_part_count < 8) {
                    target_part_t* t = &g_parts[g_part_count++];
                    t->letter = d->letter;
                    t->drv = d;
                    const char* fs = vfs_get_fs_name(d->fs_type);
                    int k = 0; while (fs[k] && k < 15) { t->fs_name[k] = fs[k]; k++; }
                    t->fs_name[k] = '\0';
                    t->total_mb = d->total_kb / 1024;
                    unsigned int tot_kb = 0, fre_kb = 0;
                    int known = 0;
                    vfs_get_drive_space(d->letter, &tot_kb, &fre_kb, &known);
                    t->free_mb = fre_kb / 1024;
                    t->is_free_known = known;
                }
            }
        }
    }
    if (g_selected_idx >= g_part_count) {
        g_selected_idx = (g_part_count > 0) ? 0 : -1;
    }
    if (g_selected_idx < 0 && g_part_count > 0) {
        g_selected_idx = 0;
    }
}

void installer_init(void) {
    g_step = STEP_SELECT;
    g_selected_idx = 0;
    g_install_error = 0;
    g_has_existing_files = 0;
    g_existing_desc[0] = '\0';
    scan_targets();
}

void installer_open(void) {
    installer_init();
    insv = 1;
    redraw = 1;
}

static void draw_btn(unsigned char* v, int bx, int by, int bw, int bh, const char* text, int bg, int fg) {
    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            PUT_PIXEL(v, x, y, bg);
        }
    }
    for (int x = bx; x < bx + bw; x++) {
        PUT_PIXEL(v, x, by, 15);
        PUT_PIXEL(v, x, by + bh - 1, 0);
    }
    for (int y = by; y < by + bh; y++) {
        PUT_PIXEL(v, bx, y, 15);
        PUT_PIXEL(v, bx + bw - 1, y, 0);
    }
    int tw = 0; while (text[tw]) tw++;
    int tx = bx + (bw - tw * 8) / 2;
    font_draw_text(v, tx, by + (bh / 2) - 4, text, fg);
}

static int copy_vfs_file(const char* src, const char* dst) {
    vfs_dirent_t ent;
    if (vfs_find_file(src, &ent) != 0) return 1;
    if (ent.size == 0) return 0;

    unsigned int pages = (ent.size + 4095) / 4096;
    unsigned char* buf = (unsigned char*)pmm_alloc_pages(pages);
    if (!buf) return 2;

    int read_bytes = vfs_read_file(src, buf, ent.size);
    if (read_bytes <= 0 || (unsigned int)read_bytes != ent.size) {
        pmm_free_pages(buf, pages);
        return 3;
    }

    int wret = vfs_write_file(dst, buf, ent.size);
    pmm_free_pages(buf, pages);
    if (wret != 0) return 4;

    return 0;
}

static int do_installation(char target_letter) {
    vfs_drive_t* d = vfs_get_drive(target_letter);
    if (!d || !d->dev || !d->is_supported) return -1;
    blkdev_t* dev = d->dev;
    unsigned int part_lba = d->start_lba;

    update_progress_ui(5, "Writing GRUB4DOS MBR to disk...", "Master Boot Record LBA 0-15");

    // 1. Write GRUB4DOS MBR to disk (LBA 0..15)
    unsigned char orig_mbr[512];
    if (dev->read_sectors(dev, 0, 1, orig_mbr) != 0) return -2;

    unsigned char new_mbr[512];
    for (int i = 0; i < 512; i++) new_mbr[i] = g_grldr_mbr[i];

    // Preserve NT disk signature (0x1B8..0x1BB), null word (0x1BC..0x1BD), and Partition Table (0x1BE..0x1FD)
    for (int i = 0x1B8; i < 0x1FE; i++) {
        new_mbr[i] = orig_mbr[i];
    }
    // Mark target partition active (0x80)
    int found_part = 0;
    for (int p = 0; p < 4; p++) {
        unsigned char* pe = &new_mbr[0x1BE + p * 16];
        unsigned int slba = *(unsigned int*)&pe[8];
        if (slba == part_lba && pe[4] != 0) {
            pe[0] = 0x80;
            found_part = 1;
        } else if (pe[4] != 0 && found_part) {
            pe[0] = 0x00;
        }
    }
    if (!found_part) {
        new_mbr[0x1BE] = 0x80;
    }
    new_mbr[0x1FE] = 0x55;
    new_mbr[0x1FF] = 0xAA;

    if (dev->write_sectors(dev, 0, 1, new_mbr) != 0) return -3;
    if (dev->write_sectors(dev, 1, 15, g_grldr_mbr + 512) != 0) return -4;

    update_progress_ui(15, "Writing GRUB4DOS PBR to partition...", "Partition Boot Record");

    // 2. Write GRUB4DOS PBR (to partition start_lba)
    unsigned char orig_pbr[512];
    if (dev->read_sectors(dev, part_lba, 1, orig_pbr) != 0) return -5;

    unsigned char new_pbr[512];
    if (d->fs_type == PART_FS_FAT32) {
        for (int i = 0; i < 512; i++) new_pbr[i] = g_grldr_pbr[i];
        // Preserve FAT32 BPB: bytes 0x03..0x59
        for (int i = 0x03; i <= 0x59; i++) new_pbr[i] = orig_pbr[i];
        new_pbr[0x1FE] = 0x55;
        new_pbr[0x1FF] = 0xAA;
        if (dev->write_sectors(dev, part_lba, 1, new_pbr) != 0) return -6;
        // Backup boot sector at sector 6
        dev->write_sectors(dev, part_lba + 6, 1, new_pbr);
    } else { // FAT12 / FAT16
        for (int i = 0; i < 512; i++) new_pbr[i] = g_grldr_pbr[512 + i];
        // Preserve FAT12/16 BPB: bytes 0x03..0x3D
        for (int i = 0x03; i <= 0x3D; i++) new_pbr[i] = orig_pbr[i];
        new_pbr[0x1FE] = 0x55;
        new_pbr[0x1FF] = 0xAA;
        if (dev->write_sectors(dev, part_lba, 1, new_pbr) != 0) return -6;
    }

    // 3. Find source CD-ROM drive letter
    char cd_let = vfs_get_boot_cdrom_letter();
    if (!cd_let) return -7;

    update_progress_ui(25, "Creating destination directories...", "Target: \\boot");

    // 4. Create destination \boot directory
    char boot_dir[16];
    boot_dir[0] = target_letter; boot_dir[1] = ':'; boot_dir[2] = '\\';
    boot_dir[3] = 'b'; boot_dir[4] = 'o'; boot_dir[5] = 'o'; boot_dir[6] = 't';
    boot_dir[7] = '\0';
    fat_create_dir(boot_dir);

    update_progress_ui(30, "Reading kernel.elf from CD-ROM...", "Loading OS image into memory");

    // 5. Copy kernel.elf
    char src_kernel[32];
    src_kernel[0] = cd_let; src_kernel[1] = ':'; src_kernel[2] = '\\';
    src_kernel[3] = 'b'; src_kernel[4] = 'o'; src_kernel[5] = 'o'; src_kernel[6] = 't';
    src_kernel[7] = '\\'; src_kernel[8] = 'k'; src_kernel[9] = 'e'; src_kernel[10] = 'r';
    src_kernel[11] = 'n'; src_kernel[12] = 'e'; src_kernel[13] = 'l'; src_kernel[14] = '.';
    src_kernel[15] = 'e'; src_kernel[16] = 'l'; src_kernel[17] = 'f'; src_kernel[18] = '\0';

    vfs_dirent_t ke_ent;
    if (vfs_find_file(src_kernel, &ke_ent) != 0) {
        src_kernel[0] = cd_let; src_kernel[1] = ':'; src_kernel[2] = '\\';
        src_kernel[3] = 'k'; src_kernel[4] = 'e'; src_kernel[5] = 'r'; src_kernel[6] = 'n';
        src_kernel[7] = 'e'; src_kernel[8] = 'l'; src_kernel[9] = '.'; src_kernel[10] = 'e';
        src_kernel[11] = 'l'; src_kernel[12] = 'f'; src_kernel[13] = '\0';
    }

    char dst_kernel[32];
    dst_kernel[0] = target_letter; dst_kernel[1] = ':'; dst_kernel[2] = '\\';
    dst_kernel[3] = 'b'; dst_kernel[4] = 'o'; dst_kernel[5] = 'o'; dst_kernel[6] = 't';
    dst_kernel[7] = '\\'; dst_kernel[8] = 'k'; dst_kernel[9] = 'e'; dst_kernel[10] = 'r';
    dst_kernel[11] = 'n'; dst_kernel[12] = 'e'; dst_kernel[13] = 'l'; dst_kernel[14] = '.';
    dst_kernel[15] = 'e'; dst_kernel[16] = 'l'; dst_kernel[17] = 'f'; dst_kernel[18] = '\0';

    fat_set_write_progress_cb(on_kernel_write_progress);
    int cp_k = copy_vfs_file(src_kernel, dst_kernel);
    fat_set_write_progress_cb(0);
    if (cp_k != 0) return -(80 + cp_k);

    update_progress_ui(78, "Copying GRUB bootloader...", "Copying grldr to root directory");

    // 6. Copy grldr
    char src_grldr[32];
    src_grldr[0] = cd_let; src_grldr[1] = ':'; src_grldr[2] = '\\';
    src_grldr[3] = 'b'; src_grldr[4] = 'o'; src_grldr[5] = 'o'; src_grldr[6] = 't';
    src_grldr[7] = '\\'; src_grldr[8] = 'g'; src_grldr[9] = 'r'; src_grldr[10] = 'l';
    src_grldr[11] = 'd'; src_grldr[12] = 'r'; src_grldr[13] = '\0';

    vfs_dirent_t de;
    if (vfs_find_file(src_grldr, &de) != 0) {
        src_grldr[0] = cd_let; src_grldr[1] = ':'; src_grldr[2] = '\\';
        src_grldr[3] = 'g'; src_grldr[4] = 'r'; src_grldr[5] = 'l';
        src_grldr[6] = 'd'; src_grldr[7] = 'r'; src_grldr[8] = '\0';
    }

    char dst_grldr_root[32];
    dst_grldr_root[0] = target_letter; dst_grldr_root[1] = ':'; dst_grldr_root[2] = '\\';
    dst_grldr_root[3] = 'g'; dst_grldr_root[4] = 'r'; dst_grldr_root[5] = 'l';
    dst_grldr_root[6] = 'd'; dst_grldr_root[7] = 'r'; dst_grldr_root[8] = '\0';

    int cp_gr = copy_vfs_file(src_grldr, dst_grldr_root);
    if (cp_gr != 0) return -91;

    char dst_grldr_boot[32];
    dst_grldr_boot[0] = target_letter; dst_grldr_boot[1] = ':'; dst_grldr_boot[2] = '\\';
    dst_grldr_boot[3] = 'b'; dst_grldr_boot[4] = 'o'; dst_grldr_boot[5] = 'o'; dst_grldr_boot[6] = 't';
    dst_grldr_boot[7] = '\\'; dst_grldr_boot[8] = 'g'; dst_grldr_boot[9] = 'r'; dst_grldr_boot[10] = 'l';
    dst_grldr_boot[11] = 'd'; dst_grldr_boot[12] = 'r'; dst_grldr_boot[13] = '\0';
    copy_vfs_file(src_grldr, dst_grldr_boot);

    update_progress_ui(88, "Writing GRUB configuration...", "Creating menu.lst");

    // 7. Write menu.lst (to root and to \boot)
    static const char g_menulst_text[] = 
        "timeout 5\r\n"
        "default 0\r\n"
        "\r\n"
        "title Pixel Neko Next\r\n"
        "find --set-root /boot/kernel.elf || find --set-root /kernel.elf\r\n"
        "kernel /boot/kernel.elf || kernel /kernel.elf\r\n"
        "boot\r\n";
    unsigned int mlen = sizeof(g_menulst_text) - 1;

    char dst_menu_root[32];
    dst_menu_root[0] = target_letter; dst_menu_root[1] = ':'; dst_menu_root[2] = '\\';
    dst_menu_root[3] = 'm'; dst_menu_root[4] = 'e'; dst_menu_root[5] = 'n'; dst_menu_root[6] = 'u';
    dst_menu_root[7] = '.'; dst_menu_root[8] = 'l'; dst_menu_root[9] = 's'; dst_menu_root[10] = 't'; dst_menu_root[11] = '\0';
    if (vfs_write_file(dst_menu_root, (const unsigned char*)g_menulst_text, mlen) != 0) return -95;

    char dst_menu_boot[32];
    dst_menu_boot[0] = target_letter; dst_menu_boot[1] = ':'; dst_menu_boot[2] = '\\';
    dst_menu_boot[3] = 'b'; dst_menu_boot[4] = 'o'; dst_menu_boot[5] = 'o'; dst_menu_boot[6] = 't';
    dst_menu_boot[7] = '\\'; dst_menu_boot[8] = 'm'; dst_menu_boot[9] = 'e'; dst_menu_boot[10] = 'n'; dst_menu_boot[11] = 'u';
    dst_menu_boot[12] = '.'; dst_menu_boot[13] = 'l'; dst_menu_boot[14] = 's'; dst_menu_boot[15] = 't'; dst_menu_boot[16] = '\0';
    vfs_write_file(dst_menu_boot, (const unsigned char*)g_menulst_text, mlen);

    update_progress_ui(95, "Flushing disk cache and syncing filesystem...", "Writing FAT FSInfo & buffers");

    // 8. Flush disk writes
    fat_flush_all();

    // 9. Refresh drive space cache
    vfs_get_drive_space(target_letter, 0, 0, 0);

    update_progress_ui(100, "Installation completed successfully!", "System files ready to boot");

    return 0;
}

void installer_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    // Fill client background
    for (int y = wy + 20; y < wy + wh; y++) {
        for (int x = wx; x < wx + ww; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }

    if (g_step == STEP_SELECT) {
        font_draw_text(v, wx + 12, wy + 26, "Welcome to OS Installation", 0);
        font_draw_text(v, wx + 12, wy + 42, "Select target drive (FAT16/FAT32):", 8);

        // List container
        int lx = wx + 12, ly = wy + 58, lw = ww - 24, lh = 110;
        for (int y = ly; y < ly + lh; y++) {
            for (int x = lx; x < lx + lw; x++) {
                PUT_PIXEL(v, x, y, 15);
            }
        }
        for (int x = lx; x < lx + lw; x++) {
            PUT_PIXEL(v, x, ly, 0);
            PUT_PIXEL(v, x, ly + lh - 1, 8);
        }
        for (int y = ly; y < ly + lh; y++) {
            PUT_PIXEL(v, lx, y, 0);
            PUT_PIXEL(v, lx + lw - 1, y, 8);
        }

        if (g_part_count == 0) {
            font_draw_text(v, lx + 12, ly + 40, "No FAT16/FAT32 hard disks found!", 12);
            font_draw_text(v, lx + 12, ly + 58, "Please prepare a FAT drive first.", 8);
        } else {
            for (int i = 0; i < g_part_count && i < 5; i++) {
                int iy = ly + 6 + i * 20;
                int is_sel = (i == g_selected_idx);
                if (is_sel) {
                    for (int y = iy - 2; y < iy + 16; y++) {
                        for (int x = lx + 4; x < lx + lw - 4; x++) {
                            PUT_PIXEL(v, x, y, 1);
                        }
                    }
                }
                // Radio button circle
                int rx = lx + 12, ry = iy + 6;
                for (int y = ry - 4; y <= ry + 4; y++) {
                    for (int x = rx - 4; x <= rx + 4; x++) {
                        int dx = x - rx, dy = y - ry;
                        if (dx*dx + dy*dy <= 16) {
                            PUT_PIXEL(v, x, y, is_sel ? 15 : 0);
                        }
                    }
                }
                if (is_sel) {
                    for (int y = ry - 2; y <= ry + 2; y++) {
                        for (int x = rx - 2; x <= rx + 2; x++) {
                            PUT_PIXEL(v, x, y, 1);
                        }
                    }
                }

                // Partition info string
                char line[64];
                line[0] = g_parts[i].letter;
                line[1] = ':'; line[2] = ' ';
                line[3] = '['; line[4] = '\0';
                str_cat_inst(line, g_parts[i].fs_name);
                str_cat_inst(line, "] ");
                char num_buf[16];
                my_itoa_inst(g_parts[i].total_mb, num_buf);
                str_cat_inst(line, num_buf);
                str_cat_inst(line, " MB");
                if (g_parts[i].is_free_known) {
                    str_cat_inst(line, " (");
                    my_itoa_inst(g_parts[i].free_mb, num_buf);
                    str_cat_inst(line, num_buf);
                    str_cat_inst(line, "M free)");
                }
                font_draw_text(v, lx + 26, iy + 2, line, is_sel ? 15 : 0);
            }
        }

        // Action buttons
        if (g_part_count > 0 && g_selected_idx >= 0) {
            draw_btn(v, wx + ww - 90, wy + wh - 30, 78, 22, "Next >", 7, 0);
        } else {
            draw_btn(v, wx + ww - 90, wy + wh - 30, 78, 22, "Next >", 8, 7);
        }
        draw_btn(v, wx + ww - 180, wy + wh - 30, 78, 22, "Cancel", 7, 0);

    } else if (g_step == STEP_OVERWRITE) {
        font_draw_text(v, wx + 12, wy + 26, "Warning: Existing Boot Files Detected", 12);

        char w_msg[64];
        w_msg[0] = 'D'; w_msg[1] = 'r'; w_msg[2] = 'i'; w_msg[3] = 'v'; w_msg[4] = 'e'; w_msg[5] = ' ';
        w_msg[6] = g_target_letter; w_msg[7] = ':'; w_msg[8] = '\\'; w_msg[9] = ' ';
        w_msg[10] = 'a'; w_msg[11] = 'l'; w_msg[12] = 'r'; w_msg[13] = 'e'; w_msg[14] = 'a'; w_msg[15] = 'd';
        w_msg[16] = 'y'; w_msg[17] = ' '; w_msg[18] = 'c'; w_msg[19] = 'o'; w_msg[20] = 'n'; w_msg[21] = 't';
        w_msg[22] = 'a'; w_msg[23] = 'i'; w_msg[24] = 'n'; w_msg[25] = 's'; w_msg[26] = ':'; w_msg[27] = '\0';
        font_draw_text(v, wx + 12, wy + 48, w_msg, 0);

        // Detected files inset box
        int bx = wx + 12, by = wy + 66, bw = ww - 24, bh = 34;
        for (int y = by; y < by + bh; y++) {
            for (int x = bx; x < bx + bw; x++) {
                PUT_PIXEL(v, x, y, 15);
            }
        }
        for (int x = bx; x < bx + bw; x++) {
            PUT_PIXEL(v, x, by, 8);
            PUT_PIXEL(v, x, by + bh - 1, 7);
        }
        for (int y = by; y < by + bh; y++) {
            PUT_PIXEL(v, bx, y, 8);
            PUT_PIXEL(v, bx + bw - 1, y, 7);
        }
        font_draw_text(v, bx + 8, by + 6, "Found: ", 8);
        font_draw_text(v, bx + 56, by + 6, g_existing_desc, 0);
        font_draw_text(v, bx + 8, by + 19, "Existing bootloader or OS files exist.", 8);

        font_draw_text(v, wx + 12, wy + 110, "Proceeding will rewrite MBR, PBR,", 0);
        font_draw_text(v, wx + 12, wy + 126, "and replace all boot & kernel files.", 0);
        font_draw_text(v, wx + 12, wy + 146, "Do you want to overwrite and install?", 12);

        draw_btn(v, wx + ww - 125, wy + wh - 30, 115, 22, "Overwrite Now", 7, 0);
        draw_btn(v, wx + ww - 210, wy + wh - 30, 75, 22, "< Back", 7, 0);
        draw_btn(v, wx + 12, wy + wh - 30, 68, 22, "Cancel", 7, 0);

    } else if (g_step == STEP_CONFIRM) {
        font_draw_text(v, wx + 12, wy + 26, "Confirm OS Installation", 0);

        char c_msg[64];
        c_msg[0] = 'I'; c_msg[1] = 'n'; c_msg[2] = 's'; c_msg[3] = 't'; c_msg[4] = 'a'; c_msg[5] = 'l'; c_msg[6] = 'l';
        c_msg[7] = ' '; c_msg[8] = 't'; c_msg[9] = 'o'; c_msg[10] = ' ';
        c_msg[11] = 'D'; c_msg[12] = 'r'; c_msg[13] = 'i'; c_msg[14] = 'v'; c_msg[15] = 'e'; c_msg[16] = ' ';
        c_msg[17] = g_target_letter; c_msg[18] = ':'; c_msg[19] = ' '; c_msg[20] = '?'; c_msg[21] = '\0';

        font_draw_text(v, wx + 12, wy + 50, c_msg, 0);
        font_draw_text(v, wx + 12, wy + 72, "The installer will perform:", 8);
        font_draw_text(v, wx + 20, wy + 90, "1. Write GRUB4DOS MBR to disk.", 0);
        font_draw_text(v, wx + 20, wy + 108, "2. Write GRUB4DOS PBR to partition.", 0);
        font_draw_text(v, wx + 20, wy + 126, "3. Copy kernel, grldr & menu.lst.", 0);
        font_draw_text(v, wx + 12, wy + 148, "Ready to proceed?", 1);

        draw_btn(v, wx + ww - 100, wy + wh - 30, 88, 22, "Install Now", 7, 0);
        draw_btn(v, wx + ww - 185, wy + wh - 30, 75, 22, "< Back", 7, 0);

    } else if (g_step == STEP_PROGRESS) {
        font_draw_text(v, wx + 12, wy + 26, "Installing Pixel Neko...", 0);
        font_draw_text(v, wx + 12, wy + 48, g_progress_text, 8);

        // Progress bar container
        int bx = wx + 12, by = wy + 72, bw = ww - 24, bh = 20;
        // Inset border
        for (int x = bx; x < bx + bw; x++) {
            PUT_PIXEL(v, x, by, 8);
            PUT_PIXEL(v, x, by + bh - 1, 15);
        }
        for (int y = by; y < by + bh; y++) {
            PUT_PIXEL(v, bx, y, 8);
            PUT_PIXEL(v, bx + bw - 1, y, 15);
        }
        // Background
        for (int y = by + 1; y < by + bh - 1; y++) {
            for (int x = bx + 1; x < bx + bw - 1; x++) {
                PUT_PIXEL(v, x, y, 15);
            }
        }
        // Fill bar
        int fill_w = ((bw - 4) * g_progress_percent) / 100;
        if (fill_w > bw - 4) fill_w = bw - 4;
        if (fill_w < 0) fill_w = 0;
        for (int y = by + 2; y < by + bh - 2; y++) {
            for (int x = bx + 2; x < bx + 2 + fill_w; x++) {
                PUT_PIXEL(v, x, y, 1);
            }
        }
        // Progress percentage text
        char pct_str[16];
        my_itoa_inst(g_progress_percent, pct_str);
        str_cat_inst(pct_str, "%");
        font_draw_text(v, bx + (bw / 2) - 12, by + 4, pct_str, (fill_w > (bw / 2)) ? 15 : 0);

        // Status subtext
        font_draw_text(v, wx + 12, wy + 102, g_progress_subtext, 0);
        font_draw_text(v, wx + 12, wy + 128, "Please do not turn off your computer.", 8);

        // Disabled button indicating background operation
        draw_btn(v, wx + ww - 100, wy + wh - 30, 88, 22, "Working...", 8, 7);

    } else if (g_step == STEP_COMPLETE) {
        if (g_install_error == 0) {
            font_draw_text(v, wx + 12, wy + 26, "Installation Succeeded!", 2);
            char ok_msg[64];
            ok_msg[0] = 'P'; ok_msg[1] = 'i'; ok_msg[2] = 'x'; ok_msg[3] = 'e'; ok_msg[4] = 'l'; ok_msg[5] = ' ';
            ok_msg[6] = 'N'; ok_msg[7] = 'e'; ok_msg[8] = 'k'; ok_msg[9] = 'o'; ok_msg[10] = ' ';
            ok_msg[11] = 'i'; ok_msg[12] = 'n'; ok_msg[13] = 's'; ok_msg[14] = 't'; ok_msg[15] = 'a'; ok_msg[16] = 'l'; ok_msg[17] = 'l'; ok_msg[18] = 'e'; ok_msg[19] = 'd';
            ok_msg[20] = ' '; ok_msg[21] = 't'; ok_msg[22] = 'o'; ok_msg[23] = ' ';
            ok_msg[24] = g_target_letter; ok_msg[25] = ':'; ok_msg[26] = '\0';
            font_draw_text(v, wx + 12, wy + 52, ok_msg, 0);
            font_draw_text(v, wx + 12, wy + 74, "GRUB4DOS MBR and PBR written.", 0);
            font_draw_text(v, wx + 12, wy + 94, "All system files copied.", 0);
            font_draw_text(v, wx + 12, wy + 124, "Restart the computer now?", 0);

            draw_btn(v, wx + 20, wy + wh - 34, 110, 24, "Restart Now", 7, 0);
            draw_btn(v, wx + ww - 130, wy + wh - 34, 110, 24, "Stay on Desktop", 7, 0);
        } else {
            font_draw_text(v, wx + 12, wy + 26, "Installation Failed!", 12);
            char err_msg[64];
            err_msg[0] = 'E'; err_msg[1] = 'r'; err_msg[2] = 'r'; err_msg[3] = 'o'; err_msg[4] = 'r';
            err_msg[5] = ' '; err_msg[6] = 'c'; err_msg[7] = 'o'; err_msg[8] = 'd'; err_msg[9] = 'e'; err_msg[10] = ':';
            err_msg[11] = ' '; err_msg[12] = '\0';
            char num_buf[16];
            my_itoa_inst(-g_install_error, num_buf);
            str_cat_inst(err_msg, num_buf);
            font_draw_text(v, wx + 12, wy + 56, err_msg, 12);

            const char* desc = "Could not write boot code or copy files.";
            if (g_install_error == -81) desc = "Source kernel not found on CD-ROM.";
            else if (g_install_error == -82) desc = "Out of memory allocating copy buffer.";
            else if (g_install_error == -83) desc = "Failed reading kernel from CD-ROM.";
            else if (g_install_error == -84) desc = "Failed writing kernel to target drive.";
            else if (g_install_error == -91) desc = "Failed to copy grldr bootloader.";
            else if (g_install_error == -95) desc = "Failed writing menu.lst configuration.";
            font_draw_text(v, wx + 12, wy + 80, desc, 0);

            draw_btn(v, wx + ww - 90, wy + wh - 30, 78, 22, "< Back", 7, 0);
        }
    }
}

void installer_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh) {
    (void)v;

    if (g_step == STEP_SELECT) {
        // Check partition selection click
        int lx = wx + 12, ly = wy + 58, lw = ww - 24;
        if (mx >= lx && mx < lx + lw && my >= ly && my < ly + 110) {
            int clicked = (my - (ly + 6)) / 20;
            if (clicked >= 0 && clicked < g_part_count) {
                g_selected_idx = clicked;
                redraw = 1;
                return;
            }
        }

        // Next > button
        int nbx = wx + ww - 90, nby = wy + wh - 30;
        if (mx >= nbx && mx < nbx + 78 && my >= nby && my < nby + 22) {
            if (g_part_count > 0 && g_selected_idx >= 0 && g_selected_idx < g_part_count) {
                g_target_letter = g_parts[g_selected_idx].letter;
                g_has_existing_files = check_existing_boot_files(g_target_letter, g_existing_desc, sizeof(g_existing_desc));
                if (g_has_existing_files > 0) {
                    g_step = STEP_OVERWRITE;
                } else {
                    g_step = STEP_CONFIRM;
                }
                redraw = 1;
            }
            return;
        }

        // Cancel button
        int cbx = wx + ww - 180, cby = wy + wh - 30;
        if (mx >= cbx && mx < cbx + 78 && my >= cby && my < cby + 22) {
            insv = 0;
            redraw = 1;
            return;
        }

    } else if (g_step == STEP_OVERWRITE) {
        // Overwrite Now button
        int obx = wx + ww - 125, oby = wy + wh - 30;
        if (mx >= obx && mx < obx + 115 && my >= oby && my < oby + 22) {
            g_step = STEP_PROGRESS;
            update_progress_ui(0, "Starting installation...", "Overwriting boot files and disk MBR");
            int err = do_installation(g_target_letter);
            g_install_error = err;
            g_step = STEP_COMPLETE;
            redraw = 1;
            return;
        }

        // < Back button
        int bbx = wx + ww - 210, bby = wy + wh - 30;
        if (mx >= bbx && mx < bbx + 75 && my >= bby && my < bby + 22) {
            g_step = STEP_SELECT;
            scan_targets();
            redraw = 1;
            return;
        }

        // Cancel button
        int cbx = wx + 12, cby = wy + wh - 30;
        if (mx >= cbx && mx < cbx + 68 && my >= cby && my < cby + 22) {
            insv = 0;
            redraw = 1;
            return;
        }

    } else if (g_step == STEP_CONFIRM) {
        // Install Now button
        int ibx = wx + ww - 100, iby = wy + wh - 30;
        if (mx >= ibx && mx < ibx + 88 && my >= iby && my < iby + 22) {
            g_has_existing_files = check_existing_boot_files(g_target_letter, g_existing_desc, sizeof(g_existing_desc));
            if (g_has_existing_files > 0) {
                g_step = STEP_OVERWRITE;
                redraw = 1;
                return;
            }
            g_step = STEP_PROGRESS;
            update_progress_ui(0, "Starting installation...", "Preparing disk and partitions");
            int err = do_installation(g_target_letter);
            g_install_error = err;
            g_step = STEP_COMPLETE;
            redraw = 1;
            return;
        }

        // < Back button
        int bbx = wx + ww - 185, bby = wy + wh - 30;
        if (mx >= bbx && mx < bbx + 75 && my >= bby && my < bby + 22) {
            g_step = STEP_SELECT;
            scan_targets();
            redraw = 1;
            return;
        }

    } else if (g_step == STEP_PROGRESS) {
        // While installing, user clicks are ignored to protect disk operations
        return;

    } else if (g_step == STEP_COMPLETE) {
        if (g_install_error == 0) {
            // Restart Now button
            int rbx = wx + 20, rby = wy + wh - 34;
            if (mx >= rbx && mx < rbx + 110 && my >= rby && my < rby + 24) {
                sys_reboot();
                return;
            }

            // Stay on Desktop button
            int sbx = wx + ww - 130, sby = wy + wh - 34;
            if (mx >= sbx && mx < sbx + 110 && my >= sby && my < sby + 24) {
                insv = 0;
                redraw = 1;
                return;
            }
        } else {
            // Back button on error
            int bbx = wx + ww - 90, bby = wy + wh - 30;
            if (mx >= bbx && mx < bbx + 78 && my >= bby && my < bby + 22) {
                g_step = STEP_SELECT;
                scan_targets();
                redraw = 1;
                return;
            }
        }
    }
}
