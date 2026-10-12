#include "filemanager.h"
#include "font.h"
#include "fat.h"
#include "disk.h"
#include "wm.h"

char current_path[256] = "C:\\";
static file_info_t files[64];
static int file_count = 0;
static int selected = -1;
static int scroll_offset = 0;
static int show_error = 0;
static int error_timer = 0;
static int last_click_idx = -1;
static int last_click_time = 0;
static int last_mb = 0;
extern volatile int global_tick;
extern int redraw;

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

static void file_callback(const file_info_t* info) {
    if (file_count < 64) {
        files[file_count] = *info;
        file_count++;
    }
}

static void load_files(void) {
    static int fat_initialized = 0;
    if (!fat_initialized) {
        fat_init();
        fat_initialized = 1;
    }
    
    file_count = 0;
    selected = -1;
    scroll_offset = 0;
    ctx_menu_open = 0;
    
    if (current_path[0] == 0) { 
        for (int i = 0; i < MAX_VOLUMES; i++) {
            if (g_volumes[i].type != 0) {
                files[file_count].attr = 0x10;
                files[file_count].size = g_volumes[i].total_clusters * g_volumes[i].sectors_per_cluster;
                files[file_count].name[0] = 'C' + i;
                files[file_count].name[1] = ':';
                files[file_count].name[2] = '\\';
                files[file_count].name[3] = 0;
                file_count++;
            }
        }
        char cdrom_letter = 'D'; // Start guessing from D
        for (int i = 0; i < MAX_VOLUMES; i++) {
            if (g_volumes[i].type != 0) cdrom_letter = 'C' + i + 1;
        }
        for (int i = 0; i < MAX_DRIVES; i++) {
            if (g_drives[i].present && g_drives[i].is_atapi) {
                files[file_count].attr = 0x10;
                files[file_count].size = 0;
                files[file_count].name[0] = cdrom_letter++;
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
    
    fat_list_dir(current_path, file_callback);
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
    if (len <= 3) { // C:\ -> drives
        current_path[0] = 0;
    } else {
        int i = len - 2; // skip trailing \ if any
        while (i > 0 && current_path[i] != '\\') i--;
        current_path[i + 1] = 0;
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
        if (len >= 4 && 
            (files[idx].name[len-1] == 't' || files[idx].name[len-1] == 'T') &&
            (files[idx].name[len-2] == 'x' || files[idx].name[len-2] == 'X') &&
            (files[idx].name[len-3] == 't' || files[idx].name[len-3] == 'T') &&
            (files[idx].name[len-4] == '.')) {
            char full[256];
            my_strcpy(full, current_path);
            int flen = my_strlen(full);
            if (flen > 0 && full[flen-1] != '\\') my_strcat(full, "\\");
            my_strcat(full, files[idx].name);
            editor_open_file(full);
            edv = 1; edm = 0;
            bring_to_top(5);
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

void fm_update(unsigned char* v, int mx, int my, int mb, int wx, int wy, int ww, int wh) {
    (void)v;
    int mb_press = (mb & 1) && !(last_mb & 1);
    int rmb_press = (mb & 2) && !(last_mb & 2);
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
    
    int max_display = (wh - 80) / 16;
    if (max_display > 10) max_display = 10;
    
    if (rmb_press) {
        ctx_target_idx = -1;
        for (int i = 0; i < max_display && (i + scroll_offset) < file_count; i++) {
            int idx = i + scroll_offset;
            int y = wy + 62 + i * 16;
            if (mx >= wx+4 && mx < wx+ww-4 && my >= y && my < y+16) {
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
    
    if (mb_press) {
        for (int i = 0; i < max_display && (i + scroll_offset) < file_count; i++) {
            int idx = i + scroll_offset;
            int y = wy + 62 + i * 16;
            
            if (mx >= wx+4 && mx < wx+ww-4 && my >= y && my < y+16) {
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

static void draw_error_dialog(unsigned char* v, int wx, int wy, int ww, int wh) {
    int dx = wx + ww/2 - 60, dy = wy + wh/2 - 25, dw = 120, dh = 50;
    for (int y = dy; y < dy+dh && y < 768; y++)
        for (int x = dx; x < dx+dw && x < 1024; x++) PUT_PIXEL(v, x, y, 8);
    for (int x = dx; x < dx+dw; x++) { PUT_PIXEL(v, x, dy, 15); PUT_PIXEL(v, x, dy+dh-1, 0); }
    for (int y = dy; y < dy+dh; y++) { PUT_PIXEL(v, dx, y, 15); PUT_PIXEL(v, dx+dw-1, y, 0); }
    for (int y = dy+10; y < dy+25; y++) for (int x = dx+10; x < dx+25; x++) PUT_PIXEL(v, x, y, 4);
    font_draw_text(v, dx+15, dy+14, "X", 15);
    font_draw_text(v, dx+30, dy+12, "Action failed", 15);
    font_draw_text(v, dx+30, dy+24, "Or unsupported.", 0);
}

void fm_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    char title[256] = "File Manager: ";
    my_strcat(title, current_path[0] ? current_path : "Drives");
    font_draw_text(v, wx+5, wy+20, title, 0);
    
    font_draw_text(v, wx+8, wy+40, "Name", 0);
    font_draw_text(v, wx+ww-75, wy+40, "Size", 0);
    for (int x = wx+4; x < wx+ww-4; x++) if (wy+58 < 768 && x < 1024) PUT_PIXEL(v, x, wy+58, 8);
    
    int max_display = (wh - 80) / 16;
    if (max_display > 10) max_display = 10;
    
    for (int i = 0; i < max_display && (i + scroll_offset) < file_count; i++) {
        int idx = i + scroll_offset;
        int y = wy + 62 + i * 16;
        if (idx == selected) {
            for (int yy = y; yy < y+16 && yy < 768; yy++)
                for (int xx = wx+4; xx < wx+ww-4 && xx < 1024; xx++) PUT_PIXEL(v, xx, yy, 9);
        }
        int fg = (idx == selected) ? 15 : 0;
        
        char dname[32] = {0};
        if (files[idx].attr & 0x10) {
            dname[0] = '['; dname[1] = 0;
            my_strcat(dname, files[idx].name);
            my_strcat(dname, "]");
        } else {
            my_strcpy(dname, files[idx].name);
        }
        font_draw_text(v, wx+8, y, dname, fg);
        
        if (current_path[0] == 0) {
            if (files[idx].size > 0) {
                char type_str[10];
                int drive_v = files[idx].name[0] - 'C';
                if (drive_v >= 0 && drive_v < MAX_VOLUMES) {
                    if (g_volumes[drive_v].type == 32) my_strcpy(type_str, "FAT32");
                    else if (g_volumes[drive_v].type == 16) my_strcpy(type_str, "FAT16");
                    else my_strcpy(type_str, "Disk");
                } else my_strcpy(type_str, "Disk");
                
                font_draw_text(v, wx+ww-150, y, type_str, fg);
                char size_str[24];
                unsigned int secs = files[idx].size;
                if (secs >= 2097152) { // >= 1GB
                    unsigned int gb = secs / 2097152;
                    int p=0; char temp[10]; int tp=0;
                    if(gb==0){size_str[p++]='0';}else{while(gb>0){temp[tp++]='0'+gb%10;gb/=10;}while(tp>0)size_str[p++]=temp[--tp];}
                    size_str[p++]=' '; size_str[p++]='G'; size_str[p++]='B'; size_str[p]=0;
                } else if (secs >= 2048) { // >= 1MB
                    unsigned int mb = secs / 2048;
                    int p=0; char temp[10]; int tp=0;
                    if(mb==0){size_str[p++]='0';}else{while(mb>0){temp[tp++]='0'+mb%10;mb/=10;}while(tp>0)size_str[p++]=temp[--tp];}
                    size_str[p++]=' '; size_str[p++]='M'; size_str[p++]='B'; size_str[p]=0;
                } else {
                    unsigned int kb = secs / 2;
                    int p=0; char temp[10]; int tp=0;
                    if(kb==0){size_str[p++]='0';}else{while(kb>0){temp[tp++]='0'+kb%10;kb/=10;}while(tp>0)size_str[p++]=temp[--tp];}
                    size_str[p++]=' '; size_str[p++]='K'; size_str[p++]='B'; size_str[p]=0;
                }
                font_draw_text(v, wx+ww-75, y, size_str, fg);
            } else {
                font_draw_text(v, wx+ww-150, y, "CD-ROM", fg);
            }
        } else if (!(files[idx].attr & 0x10)) {
            char size_str[24];
            format_size(files[idx].size, size_str);
            font_draw_text(v, wx+ww-75, y, size_str, fg);
        } else {
            font_draw_text(v, wx+ww-75, y, "DIR", fg);
        }
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