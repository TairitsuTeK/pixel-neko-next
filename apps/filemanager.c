#include "filemanager.h"
#include "font.h"
#include "fat.h"
#include "vfs.h"
#include "disk.h"
#include "wm.h"
#include "speaker.h"

#define MAX_FM_FILES 512
#define FM_HEADER_H  58
#define FM_STATUS_H  21
#define FM_ROW_H     16
#define FM_SB_W      14

char current_path[256] = "";
static file_info_t files[MAX_FM_FILES];
static int file_count = 0;
static int selected = -1;
static int scroll_offset = 0;
static int show_error = 0;
static int error_timer = 0;
static char err_msg1[36] = "Action failed";
static char err_msg2[36] = "Or unsupported.";
static int last_click_idx = -1;
static int last_click_time = 0;
static int last_mb = 0;
static int last_max_display = 10;

// Scrollbar interaction state
static int sb_dragging = 0;
static int sb_drag_start_y = 0;
static int sb_drag_start_offset = 0;
static int sb_up_pressed = 0;
static int sb_down_pressed = 0;

extern volatile int global_tick;
extern int redraw;

static void show_error_msg(const char* m1, const char* m2) {
    int i;
    for (i = 0; i < 35 && m1[i]; i++) err_msg1[i] = m1[i];
    err_msg1[i] = '\0';
    for (i = 0; i < 35 && m2[i]; i++) err_msg2[i] = m2[i];
    err_msg2[i] = '\0';
    show_error = 1;
    error_timer = 250;
    redraw = 1;
    speaker_beep();
}

static int ctx_menu_open = 0;
static int ctx_mx = 0, ctx_my = 0;
static int ctx_target_idx = -1;

static void format_size(unsigned int size, char* buf) {
    if (size < 1024) {
        int p = 0;
        if (size == 0) { buf[p++] = '0'; }
        else {
            char temp[10]; int tp = 0;
            while (size > 0) { temp[tp++] = '0' + size % 10; size /= 10; }
            while (tp > 0) buf[p++] = temp[--tp];
        }
        buf[p++] = ' '; buf[p++] = 'B'; buf[p] = 0;
    } else if (size < 1024 * 1024) {
        unsigned int kb = size / 1024;
        int p = 0;
        char temp[10]; int tp = 0;
        if (kb == 0) { buf[p++] = '0'; }
        else {
            while (kb > 0) { temp[tp++] = '0' + kb % 10; kb /= 10; }
            while (tp > 0) buf[p++] = temp[--tp];
        }
        buf[p++] = ' '; buf[p++] = 'K'; buf[p++] = 'B'; buf[p] = 0;
    } else {
        unsigned int mb = size / (1024 * 1024);
        int p = 0;
        char temp[10]; int tp = 0;
        if (mb == 0) { buf[p++] = '0'; }
        else {
            while (mb > 0) { temp[tp++] = '0' + mb % 10; mb /= 10; }
            while (tp > 0) buf[p++] = temp[--tp];
        }
        buf[p++] = ' '; buf[p++] = 'M'; buf[p++] = 'B'; buf[p] = 0;
    }
}

static void format_kb(unsigned int kb, char* buf) {
    int p = 0;
    char temp[16]; int tp = 0;
    if (kb >= 1048576) { // >= 1GB
        unsigned int gb = kb / 1048576;
        unsigned int rem = ((kb % 1048576) * 10) / 1048576;
        if (gb == 0) temp[tp++] = '0';
        else { while (gb > 0) { temp[tp++] = '0' + gb % 10; gb /= 10; } }
        while (tp > 0) buf[p++] = temp[--tp];
        buf[p++] = '.';
        buf[p++] = '0' + rem;
        buf[p++] = ' '; buf[p++] = 'G'; buf[p++] = 'B'; buf[p] = 0;
    } else if (kb >= 1024) { // >= 1MB
        unsigned int mb = kb / 1024;
        if (mb == 0) temp[tp++] = '0';
        else { while (mb > 0) { temp[tp++] = '0' + mb % 10; mb /= 10; } }
        while (tp > 0) buf[p++] = temp[--tp];
        buf[p++] = ' '; buf[p++] = 'M'; buf[p++] = 'B'; buf[p] = 0;
    } else {
        if (kb == 0) temp[tp++] = '0';
        else { while (kb > 0) { temp[tp++] = '0' + kb % 10; kb /= 10; } }
        while (tp > 0) buf[p++] = temp[--tp];
        buf[p++] = ' '; buf[p++] = 'K'; buf[p++] = 'B'; buf[p] = 0;
    }
}



static void vfs_fm_callback(const vfs_dirent_t* ent) {
    if (!ent || !ent->name[0]) return;
    // Don't add . or .. from directory listing as FM manages .. explicitly
    if (ent->name[0] == '.' && (ent->name[1] == '\0' || (ent->name[1] == '.' && ent->name[2] == '\0'))) {
        return;
    }
    if (file_count < MAX_FM_FILES) {
        int i;
        for (i = 0; i < 127 && ent->name[i]; i++) {
            files[file_count].name[i] = ent->name[i];
        }
        files[file_count].name[i] = '\0';
        files[file_count].attr = ent->is_dir ? 0x10 : 0x20;
        files[file_count].size = ent->size;
        files[file_count].first_cluster = ent->first_cluster;
        file_count++;
    }
}

static void load_files(void) {
    file_count = 0;
    selected = -1;
    scroll_offset = 0;
    ctx_menu_open = 0;
    sb_dragging = 0;
    sb_up_pressed = 0;
    sb_down_pressed = 0;
    
    if (current_path[0] == 0) { 
        // Enumerate Windows-assigned drives from VFS (A: ~ Z:)
        for (int i = 0; i < 26; i++) {
            vfs_drive_t* d = vfs_get_drive('A' + i);
            if (d && file_count < MAX_FM_FILES) {
                files[file_count].attr = 0x10;
                files[file_count].size = d->total_sectors;
                files[file_count].name[0] = d->letter;
                files[file_count].name[1] = ':';
                files[file_count].name[2] = '\\';
                files[file_count].name[3] = 0;
                file_count++;
            }
        }
        return;
    }
    
    // Add ".." entry if not at root drive list
    int len = 0; while (current_path[len]) len++;
    if (len > 0) {
        files[file_count].attr = 0x10;
        files[file_count].size = 0;
        files[file_count].name[0] = '.';
        files[file_count].name[1] = '.';
        files[file_count].name[2] = 0;
        file_count++;
    }
    
    int ret = vfs_list_dir(current_path, vfs_fm_callback);
    if (ret != VFS_OK) {
        if (ret == VFS_ERR_UNSUPPORTED) {
            show_error_msg("Cannot read partition", "File system unsupported");
        } else if (ret == VFS_ERR_NO_MEDIA) {
            show_error_msg("Device not ready", "Please insert disc/disk");
        } else {
            show_error_msg("Read failed", "Disk or read error");
        }
        if (len <= 3) {
            current_path[0] = 0;
            load_files();
        }
    }
}

void fm_init(void) {
    load_files();
}

static void my_strcpy(char* dest, const char* src) {
    while(*src) *dest++ = *src++;
    *dest = '\0';
}
static void my_strcat(char* dest, const char* src) {
    while(*dest) dest++;
    while(*src) *dest++ = *src++;
    *dest = '\0';
}
static int my_strlen(const char* s) {
    int n=0; while(s[n]) n++; return n;
}

static void go_up(void) {
    int len = my_strlen(current_path);
    if (len == 0) return; // already at Drives view

    if (len <= 3) { // At drive root (e.g. "C:\" or "C:") -> return to Drives view
        current_path[0] = 0;
    } else {
        int i = len - 1;
        if (current_path[i] == '\\' || current_path[i] == '/') {
            current_path[i] = 0;
            i--;
        }
        while (i > 0 && current_path[i] != '\\' && current_path[i] != '/') i--;
        if (i <= 2 && current_path[1] == ':') {
            current_path[i + 1] = '\\';
            current_path[i + 2] = 0;
        } else {
            current_path[i + 1] = 0;
        }
    }
    load_files();
    redraw = 1;
}

static void enter_dir(const char* name) {
    if (name[0] == '.' && name[1] == '.') {
        go_up();
        return;
    }
    if (current_path[0] == 0) {
        my_strcpy(current_path, name);
    } else {
        int len = my_strlen(current_path);
        if (current_path[len-1] != '\\') my_strcat(current_path, "\\");
        my_strcat(current_path, name);
    }
    load_files();
    redraw = 1;
}

extern int edv, edm;
void editor_open_file(const char* filename);
extern void bring_to_top(int id);

static void open_file(int idx) {
    if (files[idx].attr & 0x10) {
        enter_dir(files[idx].name);
    } else {
        int len = my_strlen(files[idx].name);
        char ext[4] = {0};
        if (len >= 4 && files[idx].name[len-4] == '.') {
            for (int k = 0; k < 3; k++) {
                char ch = files[idx].name[len-3+k];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                ext[k] = ch;
            }
        }
        if (ext[0] == 't' && ext[1] == 'x' && ext[2] == 't') {
            char full[256];
            my_strcpy(full, current_path);
            int flen = my_strlen(full);
            if (flen > 0 && full[flen-1] != '\\') my_strcat(full, "\\");
            my_strcat(full, files[idx].name);
            editor_open_file(full);
            edv = 1; edm = 0;
            bring_to_top(5);
        } else if ((ext[0] == 'b' && ext[1] == 'm' && ext[2] == 'p') ||
                   (ext[0] == 'p' && ext[1] == 'n' && ext[2] == 'g')) {
            char full[256];
            my_strcpy(full, current_path);
            int flen = my_strlen(full);
            if (flen > 0 && full[flen-1] != '\\') my_strcat(full, "\\");
            my_strcat(full, files[idx].name);
            extern void imageviewer_open_file(const char* path);
            extern int imv, imm;
            imageviewer_open_file(full);
            imv = 1; imm = 0;
            bring_to_top(8);
        } else if ((ext[0] == 'w' && ext[1] == 'a' && ext[2] == 'v') ||
                   (ext[0] == 'm' && ext[1] == 'p' && ext[2] == '3') ||
                   (len >= 5 && files[idx].name[len-5] == '.' &&
                    (files[idx].name[len-4] == 'f' || files[idx].name[len-4] == 'F') &&
                    (files[idx].name[len-3] == 'l' || files[idx].name[len-3] == 'L') &&
                    (files[idx].name[len-2] == 'a' || files[idx].name[len-2] == 'A') &&
                    (files[idx].name[len-1] == 'c' || files[idx].name[len-1] == 'C'))) {
            char full[256];
            my_strcpy(full, current_path);
            int flen = my_strlen(full);
            if (flen > 0 && full[flen-1] != '\\') my_strcat(full, "\\");
            my_strcat(full, files[idx].name);
            extern void audioplayer_open_file(const char* path);
            extern int npv, npm;
            audioplayer_open_file(full);
            npv = 1; npm = 0;
            bring_to_top(9);
        } else {
            show_error = 1;
            error_timer = 300;
        }
    }
}

static void ctx_menu_action(int act) {
    ctx_menu_open = 0;
    redraw = 1;
    if (act == 0) { // Open
        if (ctx_target_idx >= 0) open_file(ctx_target_idx);
    } else if (act == 1) { // Rename (Not fully implemented GUI, stub)
        show_error = 1; error_timer = 150; // TODO rename GUI
    } else if (act == 2) { // Delete
        if (ctx_target_idx >= 0 && current_path[0] != 0) {
            char full[256];
            my_strcpy(full, current_path);
            int flen = my_strlen(full);
            if (flen > 0 && full[flen-1] != '\\') my_strcat(full, "\\");
            my_strcat(full, files[ctx_target_idx].name);
            fat_delete(full);
            load_files();
        }
    } else if (act == 3) { // New Folder
        if (current_path[0] != 0) {
            char full[256];
            my_strcpy(full, current_path);
            int flen = my_strlen(full);
            if (flen > 0 && full[flen-1] != '\\') my_strcat(full, "\\");
            my_strcat(full, "NEW_DIR");
            fat_create_dir(full);
            load_files();
        }
    }
}

static void draw_scrollbar(unsigned char* v, int sb_x, int sb_y, int sb_w, int sb_h,
                           int scroll_offset, int max_display, int file_count) {
    if (sb_h < 30) return;

    int list_bot = sb_y + sb_h;
    int track_y = sb_y + 14;
    int track_bot = list_bot - 14;
    int track_h = track_bot - track_y;

    // 1. Left divider line (Dark gray)
    for (int y = sb_y; y < list_bot; y++) {
        PUT_PIXEL(v, sb_x - 1, y, 8);
    }

    // 2. Up Arrow Button (Height 14px)
    for (int y = sb_y; y < sb_y + 14; y++) {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }
    if (sb_up_pressed) {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, sb_y, 0);
            PUT_PIXEL(v, x, sb_y + 13, 15);
        }
        for (int y = sb_y; y < sb_y + 14; y++) {
            PUT_PIXEL(v, sb_x, y, 0);
            PUT_PIXEL(v, sb_x + sb_w - 1, y, 15);
        }
    } else {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, sb_y, 15);
            PUT_PIXEL(v, x, sb_y + 13, 0);
            PUT_PIXEL(v, x, sb_y + 12, 8);
        }
        for (int y = sb_y; y < sb_y + 14; y++) {
            PUT_PIXEL(v, sb_x, y, 15);
            PUT_PIXEL(v, sb_x + sb_w - 1, y, 0);
            PUT_PIXEL(v, sb_x + sb_w - 2, y, 8);
        }
    }
    // Up arrow icon (centered black triangle)
    int shift = sb_up_pressed ? 1 : 0;
    int uacx = sb_x + sb_w / 2 + shift;
    int uacy = sb_y + 5 + shift;
    PUT_PIXEL(v, uacx, uacy, 0);
    PUT_PIXEL(v, uacx - 1, uacy + 1, 0);
    PUT_PIXEL(v, uacx,     uacy + 1, 0);
    PUT_PIXEL(v, uacx + 1, uacy + 1, 0);
    for (int x = uacx - 2; x <= uacx + 2; x++) PUT_PIXEL(v, x, uacy + 2, 0);
    for (int x = uacx - 3; x <= uacx + 3; x++) PUT_PIXEL(v, x, uacy + 3, 0);

    // 3. Down Arrow Button (Height 14px)
    int dby = list_bot - 14;
    for (int y = dby; y < list_bot; y++) {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }
    if (sb_down_pressed) {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, dby, 0);
            PUT_PIXEL(v, x, list_bot - 1, 15);
        }
        for (int y = dby; y < list_bot; y++) {
            PUT_PIXEL(v, sb_x, y, 0);
            PUT_PIXEL(v, sb_x + sb_w - 1, y, 15);
        }
    } else {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, dby, 15);
            PUT_PIXEL(v, x, list_bot - 1, 0);
            PUT_PIXEL(v, x, list_bot - 2, 8);
        }
        for (int y = dby; y < list_bot; y++) {
            PUT_PIXEL(v, sb_x, y, 15);
            PUT_PIXEL(v, sb_x + sb_w - 1, y, 0);
            PUT_PIXEL(v, sb_x + sb_w - 2, y, 8);
        }
    }
    // Down arrow icon (centered black triangle)
    shift = sb_down_pressed ? 1 : 0;
    int dacx = sb_x + sb_w / 2 + shift;
    int dacy = dby + 5 + shift;
    for (int x = dacx - 3; x <= dacx + 3; x++) PUT_PIXEL(v, x, dacy, 0);
    for (int x = dacx - 2; x <= dacx + 2; x++) PUT_PIXEL(v, x, dacy + 1, 0);
    PUT_PIXEL(v, dacx - 1, dacy + 2, 0);
    PUT_PIXEL(v, dacx,     dacy + 2, 0);
    PUT_PIXEL(v, dacx + 1, dacy + 2, 0);
    PUT_PIXEL(v, dacx, dacy + 3, 0);

    // 4. Track Background (Classic 50% dither pattern)
    for (int y = track_y; y < track_bot; y++) {
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            int c = ((x + y) & 1) ? 15 : 7;
            PUT_PIXEL(v, x, y, c);
        }
    }

    // 5. Thumb Slider (Only if file_count > max_display)
    if (file_count > max_display && track_h > 14) {
        int max_scroll = file_count - max_display;
        int thumb_h = (track_h * max_display) / file_count;
        if (thumb_h < 14) thumb_h = 14;
        if (thumb_h > track_h) thumb_h = track_h;
        int scrollable_track = track_h - thumb_h;
        int thumb_y = track_y;
        if (max_scroll > 0 && scrollable_track > 0) {
            thumb_y = track_y + (scroll_offset * scrollable_track) / max_scroll;
        }

        // Thumb body
        for (int y = thumb_y; y < thumb_y + thumb_h; y++) {
            for (int x = sb_x; x < sb_x + sb_w; x++) {
                PUT_PIXEL(v, x, y, 7);
            }
        }
        // Thumb 3D bevel
        for (int x = sb_x; x < sb_x + sb_w; x++) {
            PUT_PIXEL(v, x, thumb_y, 15);
            PUT_PIXEL(v, x, thumb_y + thumb_h - 1, 0);
            PUT_PIXEL(v, x, thumb_y + thumb_h - 2, 8);
        }
        for (int y = thumb_y; y < thumb_y + thumb_h; y++) {
            PUT_PIXEL(v, sb_x, y, 15);
            PUT_PIXEL(v, sb_x + sb_w - 1, y, 0);
            PUT_PIXEL(v, sb_x + sb_w - 2, y, 8);
        }
        // Grip lines in center of thumb
        if (thumb_h >= 18) {
            int mid_y = thumb_y + thumb_h / 2;
            for (int off = -2; off <= 2; off += 2) {
                for (int gx = sb_x + 3; gx < sb_x + sb_w - 3; gx++) {
                    PUT_PIXEL(v, gx, mid_y + off, 15);
                    PUT_PIXEL(v, gx, mid_y + off + 1, 8);
                }
            }
        }
    }
}

void fm_update(unsigned char* v, int mx, int my, int mb, int wx, int wy, int ww, int wh) {
    (void)v;
    int mb_press = (mb & 1) && !(last_mb & 1);
    int rmb_press = (mb & 2) && !(last_mb & 2);
    int mb_down = (mb & 1);
    int mb_release = !(mb & 1) && (last_mb & 1);
    last_mb = mb;
    
    if (show_error) {
        if (mb_press) {
            int dx = wx + ww/2 - 60, dy = wy + wh/2 - 25;
            if (mx >= dx && mx < dx+120 && my >= dy && my < dy+50) { show_error = 0; redraw = 1; }
        }
        return;
    }
    
    if (ctx_menu_open) {
        if (mb_press) {
            if (mx >= ctx_mx && mx < ctx_mx + 100 && my >= ctx_my && my < ctx_my + 64) {
                int act = (my - ctx_my) / 16;
                ctx_menu_action(act);
            } else {
                ctx_menu_open = 0;
                redraw = 1;
            }
        }
        return;
    }
    
    int btn_h = 17;
    int btn_y = wy + 21;
    int btn_drv_w = 64;
    int btn_drv_x = wx + ww - 4 - FM_SB_W - btn_drv_w - 4;
    int btn_up_w = 54;
    int btn_up_x = btn_drv_x - btn_up_w - 4;

    if (mb_press) {
        if (mx >= btn_up_x && mx < btn_up_x + btn_up_w && my >= btn_y && my < btn_y + btn_h) {
            go_up();
            return;
        }
        if (mx >= btn_drv_x && mx < btn_drv_x + btn_drv_w && my >= btn_y && my < btn_y + btn_h) {
            current_path[0] = 0;
            load_files();
            redraw = 1;
            return;
        }
    }

    int list_top = wy + 60;
    int list_bot = wy + wh - FM_STATUS_H;
    int list_h = list_bot - list_top;
    int max_display = list_h / FM_ROW_H;
    if (max_display < 1) max_display = 1;
    last_max_display = max_display;

    int max_scroll = file_count - max_display;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    if (scroll_offset < 0) scroll_offset = 0;

    int sb_x = wx + ww - 4 - FM_SB_W;
    int sb_y = wy + FM_HEADER_H + 1;
    int sb_w = FM_SB_W;
    int sb_h = list_bot - sb_y;
    int track_y = sb_y + 14;
    int track_bot = sb_y + sb_h - 14;
    int track_h = track_bot - track_y;

    // Handle mouse button release
    if (mb_release || !mb_down) {
        if (sb_dragging || sb_up_pressed || sb_down_pressed) {
            sb_dragging = 0;
            sb_up_pressed = 0;
            sb_down_pressed = 0;
            redraw = 1;
        }
    }

    // Handle scrollbar thumb dragging
    if (sb_dragging && mb_down) {
        if (file_count > max_display && track_h > 14 && max_scroll > 0) {
            int thumb_h = (track_h * max_display) / file_count;
            if (thumb_h < 14) thumb_h = 14;
            if (thumb_h > track_h) thumb_h = track_h;
            int scrollable_track = track_h - thumb_h;
            if (scrollable_track > 0) {
                int dy = my - sb_drag_start_y;
                int new_offset = sb_drag_start_offset + (dy * max_scroll + (scrollable_track / 2)) / scrollable_track;
                if (new_offset < 0) new_offset = 0;
                if (new_offset > max_scroll) new_offset = max_scroll;
                if (new_offset != scroll_offset) {
                    scroll_offset = new_offset;
                    redraw = 1;
                }
            }
        }
        return;
    }

    // Scrollbar clicking
    if (mb_press && mx >= sb_x && mx < sb_x + sb_w && my >= sb_y && my < sb_y + sb_h) {
        if (my < sb_y + 14) {
            // Up Button
            sb_up_pressed = 1;
            if (scroll_offset > 0) {
                scroll_offset--;
                redraw = 1;
            }
        } else if (my >= sb_y + sb_h - 14) {
            // Down Button
            sb_down_pressed = 1;
            if (scroll_offset < max_scroll) {
                scroll_offset++;
                redraw = 1;
            }
        } else if (file_count > max_display && track_h > 14) {
            // Track / Thumb area
            int thumb_h = (track_h * max_display) / file_count;
            if (thumb_h < 14) thumb_h = 14;
            if (thumb_h > track_h) thumb_h = track_h;
            int scrollable_track = track_h - thumb_h;
            int thumb_y = track_y;
            if (max_scroll > 0 && scrollable_track > 0) {
                thumb_y = track_y + (scroll_offset * scrollable_track) / max_scroll;
            }

            if (my >= thumb_y && my < thumb_y + thumb_h) {
                // Click on Thumb -> Start dragging!
                sb_dragging = 1;
                sb_drag_start_y = my;
                sb_drag_start_offset = scroll_offset;
            } else if (my < thumb_y) {
                // Click on track above thumb -> Page Up
                scroll_offset -= max_display;
                if (scroll_offset < 0) scroll_offset = 0;
                redraw = 1;
            } else {
                // Click on track below thumb -> Page Down
                scroll_offset += max_display;
                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                redraw = 1;
            }
        }
        return;
    }
    
    // Right click for context menu (only within file list, not on scrollbar)
    if (rmb_press) {
        ctx_target_idx = -1;
        for (int i = 0; i < max_display && (i + scroll_offset) < file_count; i++) {
            int idx = i + scroll_offset;
            int y = wy + 62 + i * FM_ROW_H;
            if (mx >= wx+4 && mx < sb_x - 2 && my >= y && my < y + FM_ROW_H) {
                ctx_target_idx = idx;
                selected = idx;
                break;
            }
        }
        ctx_mx = mx; ctx_my = my;
        ctx_menu_open = 1;
        redraw = 1;
        return;
    }
    
    // Left click on file list
    if (mb_press) {
        for (int i = 0; i < max_display && (i + scroll_offset) < file_count; i++) {
            int idx = i + scroll_offset;
            int y = wy + 62 + i * FM_ROW_H;
            
            if (mx >= wx+4 && mx < sb_x - 2 && my >= y && my < y + FM_ROW_H) {
                if (last_click_idx == idx && global_tick - last_click_time < 55) {
                    open_file(idx);
                    last_click_idx = -1;
                } else {
                    selected = idx;
                    last_click_idx = idx;
                    last_click_time = global_tick;
                    redraw = 1;
                }
                return;
            }
        }
        selected = -1;
        redraw = 1;
    }
}

void fm_handle_key(int key) {
    if (show_error) {
        if (key == '\n' || key == ' ' || key == 27) {
            show_error = 0;
            redraw = 1;
        }
        return;
    }
    if (ctx_menu_open) {
        if (key == 27) {
            ctx_menu_open = 0;
            redraw = 1;
        }
        return;
    }
    if (file_count <= 0) return;

    int max_display = last_max_display;
    if (max_display < 1) max_display = 1;
    int max_scroll = file_count - max_display;
    if (max_scroll < 0) max_scroll = 0;

    if (key == 200) { // UP
        if (selected > 0) {
            selected--;
            if (selected < scroll_offset) {
                scroll_offset = selected;
            }
            redraw = 1;
        } else if (selected == -1) {
            selected = scroll_offset;
            redraw = 1;
        }
    } else if (key == 201) { // DOWN
        if (selected < file_count - 1) {
            selected++;
            if (selected >= scroll_offset + max_display) {
                scroll_offset = selected - max_display + 1;
                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            }
            redraw = 1;
        } else if (selected == -1) {
            selected = scroll_offset;
            redraw = 1;
        }
    } else if (key == 206) { // PAGE UP
        scroll_offset -= max_display;
        if (scroll_offset < 0) scroll_offset = 0;
        if (selected != -1) {
            selected -= max_display;
            if (selected < 0) selected = 0;
        }
        redraw = 1;
    } else if (key == 207) { // PAGE DOWN
        scroll_offset += max_display;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;
        if (selected != -1) {
            selected += max_display;
            if (selected >= file_count) selected = file_count - 1;
        }
        redraw = 1;
    } else if (key == 204) { // HOME
        scroll_offset = 0;
        selected = 0;
        redraw = 1;
    } else if (key == 205) { // END
        scroll_offset = max_scroll;
        selected = file_count - 1;
        redraw = 1;
    } else if (key == '\n') { // ENTER
        if (selected >= 0 && selected < file_count) {
            open_file(selected);
        }
    } else if (key == '\b') { // BACKSPACE
        go_up();
    }
}

void fm_release(void) {
    if (sb_dragging || sb_up_pressed || sb_down_pressed) {
        sb_dragging = 0;
        sb_up_pressed = 0;
        sb_down_pressed = 0;
        redraw = 1;
    }
}

static void draw_error_dialog(unsigned char* v, int wx, int wy, int ww, int wh) {
    int dw = 220, dh = 65;
    int dx = wx + (ww - dw) / 2, dy = wy + (wh - dh) / 2;
    for (int y = dy; y < dy+dh && y < screen_h; y++)
        for (int x = dx; x < dx+dw && x < screen_w; x++) PUT_PIXEL(v, x, y, 7);
    for (int x = dx; x < dx+dw; x++) { PUT_PIXEL(v, x, dy, 15); PUT_PIXEL(v, x, dy+dh-1, 0); }
    for (int y = dy; y < dy+dh; y++) { PUT_PIXEL(v, dx, y, 15); PUT_PIXEL(v, dx+dw-1, y, 0); }
    // Alert stop sign / icon box
    for (int y = dy+12; y < dy+36; y++)
        for (int x = dx+10; x < dx+34; x++) PUT_PIXEL(v, x, y, 4);
    font_draw_text(v, dx+18, dy+18, "!", 15);
    font_draw_text(v, dx+40, dy+14, err_msg1, 0);
    font_draw_text(v, dx+40, dy+28, err_msg2, 8);
    // OK button
    int obx = dx + (dw - 50) / 2, oby = dy + 44;
    for (int y = oby; y < oby+16; y++)
        for (int x = obx; x < obx+50; x++) PUT_PIXEL(v, x, y, 7);
    for (int x = obx; x < obx+50; x++) { PUT_PIXEL(v, x, oby, 15); PUT_PIXEL(v, x, oby+15, 0); }
    for (int y = oby; y < oby+16; y++) { PUT_PIXEL(v, obx, y, 15); PUT_PIXEL(v, obx+49, y, 0); }
    font_draw_text(v, obx+18, oby+4, "OK", 0);
}

void fm_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    char title[256] = "Path: ";
    my_strcat(title, current_path[0] ? current_path : "Drives (Select Disk)");
    font_draw_text(v, wx+8, wy+24, title, 0);

    // Toolbar buttons: Up and Drives
    int btn_h = 17;
    int btn_y = wy + 21;
    int btn_drv_w = 64;
    int btn_drv_x = wx + ww - 4 - FM_SB_W - btn_drv_w - 4;
    int btn_up_w = 54;
    int btn_up_x = btn_drv_x - btn_up_w - 4;

    // 1. Draw Up button
    for (int y = btn_y; y < btn_y + btn_h; y++)
        for (int x = btn_up_x; x < btn_up_x + btn_up_w; x++) PUT_PIXEL(v, x, y, 7);
    for (int x = btn_up_x; x < btn_up_x + btn_up_w; x++) { PUT_PIXEL(v, x, btn_y, 15); PUT_PIXEL(v, x, btn_y + btn_h - 1, 0); }
    for (int y = btn_y; y < btn_y + btn_h; y++) { PUT_PIXEL(v, btn_up_x, y, 15); PUT_PIXEL(v, btn_up_x + btn_up_w - 1, y, 0); }
    font_draw_text(v, btn_up_x + 10, btn_y + 4, "^ Up", 0);

    // 2. Draw Drives button
    for (int y = btn_y; y < btn_y + btn_h; y++)
        for (int x = btn_drv_x; x < btn_drv_x + btn_drv_w; x++) PUT_PIXEL(v, x, y, 7);
    for (int x = btn_drv_x; x < btn_drv_x + btn_drv_w; x++) { PUT_PIXEL(v, x, btn_y, 15); PUT_PIXEL(v, x, btn_y + btn_h - 1, 0); }
    for (int y = btn_y; y < btn_y + btn_h; y++) { PUT_PIXEL(v, btn_drv_x, y, 15); PUT_PIXEL(v, btn_drv_x + btn_drv_w - 1, y, 0); }
    font_draw_text(v, btn_drv_x + 8, btn_y + 4, "Drives", 0);

    int list_top = wy + 60;
    int list_bot = wy + wh - FM_STATUS_H;
    int list_h = list_bot - list_top;
    int max_display = list_h / FM_ROW_H;
    if (max_display < 1) max_display = 1;
    last_max_display = max_display;

    int max_scroll = file_count - max_display;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    if (scroll_offset < 0) scroll_offset = 0;

    int sb_x = wx + ww - 4 - FM_SB_W;
    int item_max_x = sb_x - 3;

    if (current_path[0] == 0) {
        font_draw_text(v, wx+8, wy+42, "Drive", 0);
        font_draw_text(v, wx+70, wy+42, "Type", 0);
        font_draw_text(v, item_max_x - 145, wy+42, "Total", 0);
        font_draw_text(v, item_max_x - 70, wy+42, "Free", 0);
    } else {
        font_draw_text(v, wx+8, wy+42, "Name", 0);
        font_draw_text(v, item_max_x - 70, wy+42, "Size", 0);
    }
    for (int x = wx+4; x < wx+ww-4; x++) if (wy+58 < screen_h && x < screen_w) PUT_PIXEL(v, x, wy+58, 8);
    
    for (int i = 0; i < max_display && (i + scroll_offset) < file_count; i++) {
        int idx = i + scroll_offset;
        int y = wy + 62 + i * FM_ROW_H;
        if (idx == selected) {
            for (int yy = y; yy < y+16 && yy < screen_h; yy++)
                for (int xx = wx+4; xx < item_max_x && xx < screen_w; xx++) PUT_PIXEL(v, xx, yy, 9);
        }
        int fg = (idx == selected) ? 15 : 0;
        
        char dname[128] = {0};
        if (files[idx].attr & 0x10) {
            dname[0] = '['; dname[1] = 0;
            if (files[idx].name[0] == '.' && files[idx].name[1] == '.' && files[idx].name[2] == '\0') {
                if (current_path[0] != 0 && my_strlen(current_path) <= 3) {
                    my_strcat(dname, ".. (Drives)");
                } else {
                    my_strcat(dname, "..");
                }
            } else {
                my_strcat(dname, files[idx].name);
            }
            my_strcat(dname, "]");
        } else {
            my_strcpy(dname, files[idx].name);
        }
        int max_chars = (item_max_x - wx - 90) / 8;
        if (max_chars > 0 && max_chars < 127) {
            int dlen = my_strlen(dname);
            if (dlen > max_chars) {
                dname[max_chars] = '\0';
                if (max_chars > 3) {
                    dname[max_chars - 1] = '.';
                    dname[max_chars - 2] = '.';
                    dname[max_chars - 3] = '.';
                }
            }
        }
        font_draw_text(v, wx+8, y, dname, fg);
        
        if (current_path[0] == 0) {
            vfs_drive_t* d = vfs_get_drive(files[idx].name[0]);
            if (d) {
                unsigned int tot_k = 0, fre_k = 0;
                int is_known = 0;
                vfs_get_drive_space(d->letter, &tot_k, &fre_k, &is_known);

                char tot_str[24], fre_str[24];
                format_kb(tot_k, tot_str);

                if (d->drv_type == VFS_DRV_FLOPPY) {
                    font_draw_text(v, wx+70, y, "FAT12", fg);
                    font_draw_text(v, item_max_x - 145, y, tot_str, fg);
                    if (is_known) {
                        format_kb(fre_k, fre_str);
                        font_draw_text(v, item_max_x - 70, y, fre_str, fg);
                    } else {
                        font_draw_text(v, item_max_x - 70, y, "--", fg);
                    }
                } else if (d->drv_type == VFS_DRV_CDROM) {
                    font_draw_text(v, wx+70, y, "CDFS", fg);
                    font_draw_text(v, item_max_x - 145, y, tot_str, fg);
                    font_draw_text(v, item_max_x - 70, y, "0 KB (RO)", fg);
                } else {
                    const char* fs_nm = vfs_get_fs_name(d->fs_type);
                    font_draw_text(v, wx+70, y, fs_nm, fg);
                    font_draw_text(v, item_max_x - 145, y, tot_str, fg);
                    if (is_known) {
                        format_kb(fre_k, fre_str);
                        font_draw_text(v, item_max_x - 70, y, fre_str, fg);
                    } else {
                        font_draw_text(v, item_max_x - 70, y, "-- (N/A)", fg);
                    }
                }
            } else {
                font_draw_text(v, wx+70, y, "Drive", fg);
            }
        } else if (!(files[idx].attr & 0x10)) {
            char size_str[24];
            format_size(files[idx].size, size_str);
            font_draw_text(v, item_max_x - 70, y, size_str, fg);
        } else {
            font_draw_text(v, item_max_x - 70, y, "DIR", fg);
        }
    }
    
    // Draw the vertical scrollbar on the right side
    int sb_y = wy + FM_HEADER_H + 1;
    int sb_h = list_bot - sb_y;
    draw_scrollbar(v, sb_x, sb_y, FM_SB_W, sb_h, scroll_offset, max_display, file_count);

    // Bottom status bar
    int status_y = wy + wh - 18;
    for (int x = wx+4; x < wx+ww-4; x++) PUT_PIXEL(v, x, status_y - 3, 8);
    char status_txt[128];
    if (current_path[0] == 0) {
        font_draw_text(v, wx + 8, status_y, "Drives Root | Double-click to open drive", 8);
    } else {
        char cur_drv = current_path[0];
        unsigned int tot_k = 0, fre_k = 0; int is_known = 0;
        vfs_get_drive_space(cur_drv, &tot_k, &fre_k, &is_known);
        char t_str[24], f_str[24];
        format_kb(tot_k, t_str);
        status_txt[0] = 0;
        my_strcat(status_txt, "Drive ");
        char dstr[4] = { cur_drv, ':', '\\', 0 };
        my_strcat(status_txt, dstr);
        my_strcat(status_txt, " | Total: ");
        my_strcat(status_txt, t_str);
        if (is_known) {
            format_kb(fre_k, f_str);
            my_strcat(status_txt, " | Free: ");
            my_strcat(status_txt, f_str);
        }
        int max_status_chars = (ww - 20) / 8;
        if (max_status_chars > 0 && max_status_chars < 127) {
            status_txt[max_status_chars] = '\0';
        }
        font_draw_text(v, wx + 8, status_y, status_txt, 0);
    }
    
    static int last_err_tick = 0;
    int err_dt = global_tick - last_err_tick;
    if (err_dt < 0) err_dt = 0;
    if (err_dt > 0) {
        last_err_tick = global_tick;
        if (show_error && error_timer > 0) {
            error_timer -= err_dt;
            if (error_timer <= 0) {
                error_timer = 0;
                show_error = 0;
                extern int redraw;
                redraw = 1;
            }
        }
    }
    if (show_error && error_timer > 0) {
        draw_error_dialog(v, wx, wy, ww, wh);
    }
    
    if (ctx_menu_open) {
        for (int y = ctx_my; y < ctx_my + 64 && y < 768; y++)
            for (int x = ctx_mx; x < ctx_mx + 100 && x < 1024; x++) PUT_PIXEL(v, x, y, 7);
        for (int x = ctx_mx; x < ctx_mx+100; x++) { PUT_PIXEL(v, x, ctx_my, 15); PUT_PIXEL(v, x, ctx_my+63, 0); }
        for (int y = ctx_my; y < ctx_my+64; y++) { PUT_PIXEL(v, ctx_mx, y, 15); PUT_PIXEL(v, ctx_mx+99, y, 0); }
        font_draw_text(v, ctx_mx + 8, ctx_my + 2, "Open", 0);
        font_draw_text(v, ctx_mx + 8, ctx_my + 18, "Rename", 0);
        font_draw_text(v, ctx_mx + 8, ctx_my + 34, "Delete", 0);
        font_draw_text(v, ctx_mx + 8, ctx_my + 50, "New Folder", 0);
    }
}