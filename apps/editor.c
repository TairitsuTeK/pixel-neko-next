#include "editor.h"
#include "font.h"
#include "fat.h"
#include "vfs.h"
#include "wm.h"
#include "keyboard.h"

#define EDITOR_MAX 65536
static char editor_buf[EDITOR_MAX];
static int editor_len = 0;
static int cursor_pos = 0;
static int scroll_y = 0;
static int scroll_x = 0;

static char current_filename[256] = {0};
static int is_modified = 0;

static int save_dialog = 0;
static char save_input[256] = {0};
static int save_input_len = 0;

static char clipboard[16384] = {0};
static int clipboard_len = 0;

static int sel_start = -1;
static int sel_end = -1;

void editor_init(void) {
    editor_len = 0;
    cursor_pos = 0;
    scroll_y = 0;
    scroll_x = 0;
    current_filename[0] = 0;
    is_modified = 0;
    save_dialog = 0;
    sel_start = -1;
    sel_end = -1;
}

void editor_open_file(const char* filename) {
    editor_init();
    int bytes = vfs_read_file(filename, (unsigned char*)editor_buf, EDITOR_MAX);
    if (bytes >= 0) {
        editor_len = bytes;
        for (int i = 0; i < 256; i++) {
            current_filename[i] = filename[i];
            if (!filename[i]) break;
        }
    }
}

#include "speaker.h"

static void save_file(const char* name) {
    int ret = vfs_write_file(name, (const unsigned char*)editor_buf, editor_len);
    if (ret == VFS_OK) {
        is_modified = 0;
        for (int i = 0; i < 256; i++) {
            current_filename[i] = name[i];
            if (!name[i]) break;
        }
    } else {
        speaker_beep();
    }
}

void editor_start_save(void) {
    if (current_filename[0] != 0 && is_modified) {
        save_file(current_filename);
    } else {
        save_dialog = 1;
        save_input_len = 0;
        for(int i=0;i<256;i++) {
            save_input[i]=current_filename[i];
            if(!current_filename[i]){ save_input_len=i; break; }
        }
        if (save_input_len == 0) {
            save_input[0] = 'C';
            save_input[1] = ':';
            save_input[2] = '\\';
            save_input[3] = 0;
            save_input_len = 3;
        }
    }
}

static void delete_selection(void) {
    if (sel_start == -1 || sel_end == -1) return;
    int s = sel_start < sel_end ? sel_start : sel_end;
    int e = sel_start > sel_end ? sel_start : sel_end;
    if (s == e) return;
    int len = e - s;
    for (int i = e; i < editor_len; i++) {
        editor_buf[i - len] = editor_buf[i];
    }
    editor_len -= len;
    cursor_pos = s;
    sel_start = -1;
    sel_end = -1;
    is_modified = 1;
}

static void insert_char(char c) {
    if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
        delete_selection();
    }
    if (editor_len < EDITOR_MAX - 1) {
        for (int i = editor_len; i > cursor_pos; i--) {
            editor_buf[i] = editor_buf[i-1];
        }
        editor_buf[cursor_pos] = c;
        cursor_pos++;
        editor_len++;
        is_modified = 1;
    }
}

static int get_line_start(int pos) {
    int p = pos;
    while (p > 0 && editor_buf[p-1] != '\n') p--;
    return p;
}

static int get_line_end(int pos) {
    int p = pos;
    while (p < editor_len && editor_buf[p] != '\n') p++;
    return p;
}

static void get_xy_from_pos(int pos, int* out_x, int* out_y) {
    int x = 0, y = 0;
    for (int i = 0; i < pos; i++) {
        if (editor_buf[i] == '\n') {
            y++;
            x = 0;
        } else {
            x++;
        }
    }
    *out_x = x;
    *out_y = y;
}

static int get_pos_from_xy(int tx, int ty) {
    int cur_x = 0, cur_y = 0;
    for (int i = 0; i < editor_len; i++) {
        if (cur_y == ty) {
            if (cur_x == tx) return i;
        }
        if (editor_buf[i] == '\n') {
            if (cur_y == ty) return i;
            cur_y++;
            cur_x = 0;
        } else {
            cur_x++;
        }
    }
    if (cur_y == ty) return editor_len;
    if (cur_y < ty) return editor_len;
    return 0;
}

void editor_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    // Background
    for (int y = wy; y < wy + wh; y++) {
        for (int x = wx; x < wx + ww; x++) {
            if (x < screen_w && y < screen_h) PUT_PIXEL(v, x, y, 15);
        }
    }

    int cx, cy;
    get_xy_from_pos(cursor_pos, &cx, &cy);
    
    // Auto scroll
    int max_visible_lines = (wh - 24) / 16; // Bottom 24px reserved for status
    int max_visible_cols = ww / 8;
    if (cy < scroll_y) scroll_y = cy;
    if (cy >= scroll_y + max_visible_lines) scroll_y = cy - max_visible_lines + 1;
    if (cx < scroll_x) scroll_x = cx;
    if (cx >= scroll_x + max_visible_cols) scroll_x = cx - max_visible_cols + 1;
    
    int s1 = sel_start < sel_end ? sel_start : sel_end;
    int s2 = sel_start > sel_end ? sel_start : sel_end;

    // Draw text
    int cur_x = 0, cur_y = 0;
    for (int i = 0; i <= editor_len; i++) {
        int px = wx + (cur_x - scroll_x) * 8;
        int py = wy + (cur_y - scroll_y) * 16;
        
        // Draw selection highlight
        if (sel_start != -1 && i >= s1 && i < s2) {
            if (py >= wy && py + 16 < wy + wh - 24) {
                for(int hy=py; hy<py+16; hy++)
                    for(int hx=px; hx<px+8; hx++)
                        if(hx >= wx && hx < wx+ww) PUT_PIXEL(v, hx, hy, 9);
            }
        }
        
        // Draw cursor
        if (i == cursor_pos && !save_dialog) {
            if (px >= wx && px < wx + ww && py >= wy && py + 16 < wy + wh - 24) {
                for (int cyy = py; cyy < py + 16; cyy++) PUT_PIXEL(v, px, cyy, 0);
            }
        }
        
        if (i < editor_len) {
            char c = editor_buf[i];
            if (c == '\n') {
                cur_y++;
                cur_x = 0;
            } else {
                int fg = (sel_start != -1 && i >= s1 && i < s2) ? 15 : 0;
                if (px >= wx && px + 8 <= wx + ww && py >= wy && py + 16 < wy + wh - 24) {
                    font_draw_char(v, px, py, c, fg);
                }
                cur_x++;
            }
        }
    }
    
    // Status bar
    int sby = wy + wh - 24;
    for (int x = wx; x < wx + ww; x++) {
        if (x < screen_w && sby < screen_h) PUT_PIXEL(v, x, sby, 8);
    }
    char status[64];
    int p = 0;
    const char* nm = current_filename[0] ? current_filename : "Untitled";
    while (*nm) status[p++] = *nm++;
    if (is_modified) status[p++] = '*';
    status[p] = 0;
    font_draw_text(v, wx + 4, sby + 4, status, 0);
    
    // Save dialog
    if (save_dialog) {
        int dx = wx + ww/2 - 80;
        int dy = wy + wh/2 - 30;
        for (int yy=dy; yy<dy+60; yy++)
            for (int xx=dx; xx<dx+160; xx++)
                if(xx<screen_w && yy<screen_h) PUT_PIXEL(v, xx, yy, 7);
        for(int xx=dx; xx<dx+160; xx++) { PUT_PIXEL(v, xx, dy, 15); PUT_PIXEL(v, xx, dy+59, 0); }
        for(int yy=dy; yy<dy+60; yy++) { PUT_PIXEL(v, dx, yy, 15); PUT_PIXEL(v, dx+159, yy, 0); }
        
        font_draw_text(v, dx + 10, dy + 10, "Path (C:\\A.TXT):", 0);
        for (int yy=dy+30; yy<dy+46; yy++)
            for (int xx=dx+10; xx<dx+150; xx++)
                PUT_PIXEL(v, xx, yy, 15);
        font_draw_text(v, dx + 12, dy + 30, save_input, 0);
        // cursor
        for (int yy=dy+30; yy<dy+46; yy++) PUT_PIXEL(v, dx + 12 + save_input_len*8, yy, 0);
    }
}

void editor_handle_key(int key) {
    if (save_dialog) {
        if (key == 27) { // ESC
            save_dialog = 0;
        } else if (key == '\n') { // Enter
            if (save_input_len > 0) {
                save_file(save_input);
                save_dialog = 0;
            }
        } else if (key == 0x08 && save_input_len > 0) { // Backspace
            save_input_len--;
            save_input[save_input_len] = 0;
        } else if (key >= 32 && key <= 126 && save_input_len < 255) {
            save_input[save_input_len++] = (char)key;
            save_input[save_input_len] = 0;
        }
        return;
    }
    
    int ctrl = keyboard_get_ctrl();
    if (ctrl) {
        if (key == 's' || key == 'S') {
            if (current_filename[0]) {
                save_file(current_filename);
            } else {
                save_dialog = 1;
                save_input_len = 0;
                save_input[0] = 0;
            }
        } else if (key == 'a' || key == 'A') {
            sel_start = 0;
            sel_end = editor_len;
            cursor_pos = editor_len;
        } else if (key == 'c' || key == 'C') {
            if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
                int s1 = sel_start < sel_end ? sel_start : sel_end;
                int s2 = sel_start > sel_end ? sel_start : sel_end;
                clipboard_len = s2 - s1;
                if (clipboard_len > 16384) clipboard_len = 16384;
                for (int i=0; i<clipboard_len; i++) clipboard[i] = editor_buf[s1+i];
            }
        } else if (key == 'x' || key == 'X') {
            if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
                int s1 = sel_start < sel_end ? sel_start : sel_end;
                int s2 = sel_start > sel_end ? sel_start : sel_end;
                clipboard_len = s2 - s1;
                if (clipboard_len > 16384) clipboard_len = 16384;
                for (int i=0; i<clipboard_len; i++) clipboard[i] = editor_buf[s1+i];
                delete_selection();
            }
        } else if (key == 'v' || key == 'V') {
            delete_selection();
            for (int i=0; i<clipboard_len; i++) {
                insert_char(clipboard[i]);
            }
        }
        return;
    }
    
    if (key == 200) { // UP
        int cx, cy; get_xy_from_pos(cursor_pos, &cx, &cy);
        if (cy > 0) cursor_pos = get_pos_from_xy(cx, cy - 1);
        sel_start = sel_end = -1;
    } else if (key == 201) { // DOWN
        int cx, cy; get_xy_from_pos(cursor_pos, &cx, &cy);
        cursor_pos = get_pos_from_xy(cx, cy + 1);
        sel_start = sel_end = -1;
    } else if (key == 202) { // LEFT
        if (cursor_pos > 0) cursor_pos--;
        sel_start = sel_end = -1;
    } else if (key == 203) { // RIGHT
        if (cursor_pos < editor_len) cursor_pos++;
        sel_start = sel_end = -1;
    } else if (key == 204) { // HOME
        cursor_pos = get_line_start(cursor_pos);
        sel_start = sel_end = -1;
    } else if (key == 205) { // END
        cursor_pos = get_line_end(cursor_pos);
        sel_start = sel_end = -1;
    } else if (key == '\b') { // Backspace
        if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
            delete_selection();
        } else if (cursor_pos > 0) {
            for (int i = cursor_pos; i < editor_len; i++) {
                editor_buf[i-1] = editor_buf[i];
            }
            cursor_pos--;
            editor_len--;
            is_modified = 1;
        }
    } else if (key == 208) { // Del
        if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
            delete_selection();
        } else if (cursor_pos < editor_len) {
            for (int i = cursor_pos + 1; i < editor_len; i++) {
                editor_buf[i-1] = editor_buf[i];
            }
            editor_len--;
            is_modified = 1;
        }
    } else if (key >= 32 && key <= 126) {
        insert_char((char)key);
    } else if (key == '\n') {
        insert_char('\n');
    }
}

void editor_click(int mx, int my, int wx, int wy, int ww, int wh) {
    (void)ww; (void)wh;
    if (save_dialog) return;
    if (my < wy + wh - 24) {
        int tx = scroll_x + (mx - wx) / 8;
        int ty = scroll_y + (my - wy) / 16;
        cursor_pos = get_pos_from_xy(tx, ty);
        sel_start = cursor_pos;
        sel_end = cursor_pos;
    }
}

void editor_drag(int mx, int my, int wx, int wy, int ww, int wh) {
    (void)ww; (void)wh;
    if (save_dialog) return;
    if (my < wy + wh - 24 && sel_start != -1) {
        int tx = scroll_x + (mx - wx) / 8;
        int ty = scroll_y + (my - wy) / 16;
        int p = get_pos_from_xy(tx, ty);
        if (tx < 0) p = get_line_start(get_pos_from_xy(0, ty));
        cursor_pos = p;
        sel_end = cursor_pos;
    }
}

void editor_release(void) {
    if (sel_start == sel_end) {
        sel_start = -1;
        sel_end = -1;
    }
}

char g_clipboard[4096] = {0};

void editor_copy(void) {
    if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
        int s = sel_start < sel_end ? sel_start : sel_end;
        int e = sel_start > sel_end ? sel_start : sel_end;
        int len = e - s;
        if (len >= (int)sizeof(g_clipboard) - 1) len = sizeof(g_clipboard) - 1;
        for (int i = 0; i < len; i++) {
            g_clipboard[i] = editor_buf[s + i];
        }
        g_clipboard[len] = '\0';
    }
}

void editor_cut(void) {
    editor_copy();
    delete_selection();
}

void editor_paste(void) {
    if (g_clipboard[0] == '\0') return;
    if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
        delete_selection();
    }
    for (int i = 0; g_clipboard[i] != '\0'; i++) {
        insert_char(g_clipboard[i]);
    }
}
