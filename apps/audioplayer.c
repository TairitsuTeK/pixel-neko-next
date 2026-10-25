// apps/audioplayer.c - NekoPlayer 复古 Hi-Fi 音频播放器
#include "audioplayer.h"
#include "audio.h"
#include "font.h"
#include "wm.h"

extern volatile int global_tick;

// 播放器状态与控件
static char current_title[128] = "NekoPlayer - No Disc";
static int marquee_pos = 0;
static int marquee_timer = 0;
static int is_dragging_seek = 0;
static int is_dragging_vol = 0;
static int g_drag_seek_target_sec = -1;
static char g_last_opened_filepath[256] = {0};

static void my_strcpy(char* d, const char* s) {
    while (*s) *d++ = *s++;
    *d = '\0';
}

static void draw_wrapped_text(unsigned char* v, int x, int y, int max_chars,
                              int max_lines, const char* text,
                              unsigned char color) {
    char line[64];
    int pos = 0;
    int line_no = 0;

    if (max_chars < 4) max_chars = 4;
    if (max_chars > 61) max_chars = 61;

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
}

static void my_itoa_2d(int n, char* buf) {
    if (n < 0) n = 0;
    if (n > 99) n = 99;
    buf[0] = (char)('0' + (n / 10));
    buf[1] = (char)('0' + (n % 10));
    buf[2] = '\0';
}

void audioplayer_init(void) {
    marquee_pos = 0;
    marquee_timer = 0;
    is_dragging_seek = 0;
    is_dragging_vol = 0;
    g_drag_seek_target_sec = -1;
    if (!audio_has_hardware()) {
        my_strcpy(current_title, "No Output Device (No Audio Hardware)");
    } else {
        my_strcpy(current_title, "NekoPlayer ready. Open a file to play.");
    }
}

const char* audioplayer_get_title(void) {
    return "NekoPlayer";
}

void audioplayer_open_file(const char* filepath) {
    if (!filepath || !filepath[0]) return;
    
    // 提取纯文件名用于默认显示
    const char* fn = filepath;
    for (int i = 0; filepath[i]; i++) {
        if (filepath[i] == '\\' || filepath[i] == '/') {
            fn = &filepath[i + 1];
        }
    }
    my_strcpy(current_title, fn);
    my_strcpy(g_last_opened_filepath, filepath);
    marquee_pos = 0;
    
    // 启动内核音频播放管线
    audio_play_file(filepath);
    
    // 如果读取到更详细的元数据，更新标题
    const char* track_title = audio_get_title();
    if (track_title && track_title[0]) {
        my_strcpy(current_title, track_title);
    }
}

// 绘制立体感复古按钮
static void draw_retro_btn(unsigned char* v, int bx, int by, int bw, int bh, const char* label, int pressed, int disabled) {
    unsigned char bg = disabled ? 7 : (pressed ? 8 : 7);
    unsigned char light = disabled ? 15 : (pressed ? 0 : 15);
    unsigned char shadow = disabled ? 8 : (pressed ? 15 : 0);
    
    // 填充底色
    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            PUT_PIXEL(v, x, y, bg);
        }
    }
    // 顶面和左侧高光
    for (int x = bx; x < bx + bw; x++) {
        PUT_PIXEL(v, x, by, light);
    }
    for (int y = by; y < by + bh; y++) {
        PUT_PIXEL(v, bx, y, light);
    }
    // 底面和右侧阴影
    for (int x = bx; x < bx + bw; x++) {
        PUT_PIXEL(v, x, by + bh - 1, shadow);
    }
    for (int y = by; y < by + bh; y++) {
        PUT_PIXEL(v, bx + bw - 1, y, shadow);
    }
    
    // 居中文字
    int len = 0;
    while (label[len]) len++;
    int tx = bx + (bw - len * 8) / 2;
    int ty = by + (bh - 12) / 2;
    if (pressed) { tx++; ty++; }
    
    unsigned char text_color = disabled ? 8 : 0;
    font_draw_text(v, tx, ty, label, text_color);
}

void audioplayer_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    // Fill only the client area. The old fixed 340x185 rectangle covered the
    // window manager's right and bottom borders, making the frame look broken.
    int body_left = wx + 1;
    int body_top = wy + 19;
    int body_right = wx + ww - 1;
    int body_bottom = wy + wh - 1;
    for (int y = body_top; y < body_bottom; y++) {
        for (int x = body_left; x < body_right; x++) {
            unsigned char bg_c = ((x + y) & 1) ? 7 : 8;
            PUT_PIXEL(v, x, y, bg_c);
        }
    }
    
    // =========================================================================
    // 1. LCD 荧光显示屏区域（墨绿复古液晶质感，X: wx+10, Y: wy+25, W: 320, H: 68）
    // =========================================================================
    int lcd_x = wx + 10;
    int lcd_y = wy + 25;
    int lcd_w = 320;
    int lcd_h = 68;
    
    // LCD 外边框内凹立体效果
    for (int x = lcd_x - 1; x <= lcd_x + lcd_w; x++) {
        PUT_PIXEL(v, x, lcd_y - 1, 0);
        PUT_PIXEL(v, x, lcd_y + lcd_h, 15);
    }
    for (int y = lcd_y - 1; y <= lcd_y + lcd_h; y++) {
        PUT_PIXEL(v, lcd_x - 1, y, 0);
        PUT_PIXEL(v, lcd_x + lcd_w, y, 15);
    }
    
    // LCD 内部荧光绿黑色底色（调色板色号：0 为黑，2 为深绿，10 为鲜绿）
    for (int y = lcd_y; y < lcd_y + lcd_h; y++) {
        for (int x = lcd_x; x < lcd_x + lcd_w; x++) {
            PUT_PIXEL(v, x, y, 0); // 极深墨黑底色
        }
    }
    
    int dev_ok = audio_has_hardware();
    if (!dev_ok) {
        // ---------------------------------------------------------------------
        // 【关键需求】：未识别到声卡，提示“无输出设备”！
        // ---------------------------------------------------------------------
        int blink = (global_tick / 35) & 1;
        unsigned char warn_color = blink ? 12 : 4; // 醒目红色闪烁
        font_draw_text(v, lcd_x + 14, lcd_y + 8, "[!] NO OUTPUT DEVICE", warn_color);
        font_draw_text(v, lcd_x + 14, lcd_y + 26, "Supported PCI Audio Card", 14); // 黄色警告
        font_draw_text(v, lcd_x + 14, lcd_y + 44, "Hardware not detected on bus", 8); // 暗灰
    } else {
        // 声卡正常，显示状态、跑马灯、参数与动效
        audio_state_t st = audio_get_state();
        const char* st_tag = "[STOP]";
        unsigned char st_col = 8;
        if (st == AUDIO_STATE_PLAYING) {
            st_tag = "[PLAY]";
            st_col = 10; // 亮绿
        } else if (st == AUDIO_STATE_PAUSED) {
            st_tag = "[PAUS]";
            st_col = 14; // 黄色
        }
        font_draw_text(v, lcd_x + 6, lcd_y + 6, st_tag, st_col);
        
        // 动态时间码显示：[MM:SS / MM:SS]
        unsigned int cur_sec = (is_dragging_seek && g_drag_seek_target_sec >= 0) ? (unsigned int)g_drag_seek_target_sec : audio_get_position_sec();
        unsigned int tot_sec = audio_get_duration_sec();
        
        char time_str[24];
        char cur_m[4], cur_s[4], tot_m[4], tot_s[4];
        my_itoa_2d((int)(cur_sec / 60), cur_m);
        my_itoa_2d((int)(cur_sec % 60), cur_s);
        if (tot_sec > 0) {
            my_itoa_2d((int)(tot_sec / 60), tot_m);
            my_itoa_2d((int)(tot_sec % 60), tot_s);
        } else {
            tot_m[0] = '-'; tot_m[1] = '-'; tot_m[2] = '\0';
            tot_s[0] = '-'; tot_s[1] = '-'; tot_s[2] = '\0';
        }
        
        time_str[0] = cur_m[0]; time_str[1] = cur_m[1]; time_str[2] = ':';
        time_str[3] = cur_s[0]; time_str[4] = cur_s[1]; time_str[5] = ' ';
        time_str[6] = '/'; time_str[7] = ' ';
        time_str[8] = tot_m[0]; time_str[9] = tot_m[1]; time_str[10] = ':';
        time_str[11] = tot_s[0]; time_str[12] = tot_s[1]; time_str[13] = '\0';
        font_draw_text(v, lcd_x + 60, lcd_y + 6, time_str, 10);
        
        // 声卡硬件采样率信息指示
        font_draw_text(v, lcd_x + 180, lcd_y + 6, "44.1k/48k DSP", 9);
        
        // 跑马灯显示曲目信息（LCD 居中行）
        char marquee_buf[36];
        int tlen = 0;
        while (current_title[tlen]) tlen++;
        
        if (tlen <= 30) {
            font_draw_text(v, lcd_x + 6, lcd_y + 24, current_title, 15);
        } else {
            // 滚动计算
            marquee_timer++;
            if (marquee_timer > 5) {
                marquee_timer = 0;
                marquee_pos++;
                if (marquee_pos >= tlen + 6) marquee_pos = 0;
            }
            for (int ci = 0; ci < 30; ci++) {
                int idx = marquee_pos + ci;
                if (idx < tlen) {
                    marquee_buf[ci] = current_title[idx];
                } else if (idx < tlen + 4) {
                    marquee_buf[ci] = ' ';
                } else {
                    int wrap_idx = idx - (tlen + 4);
                    marquee_buf[ci] = (wrap_idx < tlen) ? current_title[wrap_idx] : ' ';
                }
            }
            marquee_buf[30] = '\0';
            font_draw_text(v, lcd_x + 6, lcd_y + 24, marquee_buf, 15);
        }
        
        // ---------------------------------------------------------------------
        // 10 段频谱仪（Spectrum Analyzer，底部，LCD_X+6 到 LCD_X+170）
        // ---------------------------------------------------------------------
        unsigned char spec[10];
        audio_get_spectrum(spec);
        for (int bi = 0; bi < 10; bi++) {
            int bar_x = lcd_x + 6 + bi * 16;
            int bar_val = spec[bi]; // 0 ~ 15
            if (bar_val > 15) bar_val = 15;
            
            // 绘制每条柱的刻度点阵
            for (int seg = 0; seg < 15; seg++) {
                int seg_y = lcd_y + 62 - seg * 2;
                unsigned char col = 0;
                if (seg < bar_val) {
                    if (seg < 8) col = 2;       // 绿色低段
                    else if (seg < 12) col = 14; // 黄色中高段
                    else col = 12;              // 红色峰值段
                } else {
                    col = 8; // 熄灭格暗灰
                }
                for (int bx = bar_x; bx < bar_x + 12; bx++) {
                    PUT_PIXEL(v, bx, seg_y, col);
                }
            }
        }
        
        // ---------------------------------------------------------------------
        // 立体声双声道 VU 表（右下部，LCD_X+180 到 LCD_X+310）
        // ---------------------------------------------------------------------
        unsigned char vu_l, vu_r;
        audio_get_vu(&vu_l, &vu_r);
        font_draw_text(v, lcd_x + 176, lcd_y + 44, "L", 10);
        font_draw_text(v, lcd_x + 176, lcd_y + 54, "R", 10);
        
        int vu_len_l = (vu_l * 120) / 255;
        int vu_len_r = (vu_r * 120) / 255;
        for (int vx = 0; vx < 120; vx++) {
            unsigned char col_l = (vx < vu_len_l) ? ((vx > 100) ? 12 : ((vx > 75) ? 14 : 10)) : 8;
            unsigned char col_r = (vx < vu_len_r) ? ((vx > 100) ? 12 : ((vx > 75) ? 14 : 10)) : 8;
            for (int py = lcd_y + 45; py <= lcd_y + 49; py++) PUT_PIXEL(v, lcd_x + 190 + vx, py, col_l);
            for (int py = lcd_y + 55; py <= lcd_y + 59; py++) PUT_PIXEL(v, lcd_x + 190 + vx, py, col_r);
        }
    }
    
    // =========================================================================
    // 2. 播放进度条（Seek Bar，X: wx+10, Y: wy+100, W: 320, H: 10）
    // =========================================================================
    int sb_x = wx + 10;
    int sb_y = wy + 100;
    int sb_w = 320;
    int sb_h = 10;
    
    // 进度条槽凹槽
    for (int x = sb_x; x < sb_x + sb_w; x++) {
        for (int y = sb_y; y < sb_y + sb_h; y++) {
            PUT_PIXEL(v, x, y, 0);
        }
    }
    for (int x = sb_x; x < sb_x + sb_w; x++) {
        PUT_PIXEL(v, x, sb_y, 8);
        PUT_PIXEL(v, x, sb_y + sb_h - 1, 15);
    }
    
    // 计算播放比例
    unsigned int c_sec = (is_dragging_seek && g_drag_seek_target_sec >= 0) ? (unsigned int)g_drag_seek_target_sec : audio_get_position_sec();
    unsigned int d_sec = audio_get_duration_sec();
    int prog_w = 0;
    if (d_sec > 0) {
        prog_w = (int)((c_sec * (unsigned int)(sb_w - 6)) / d_sec);
        if (prog_w > sb_w - 6) prog_w = sb_w - 6;
    }
    // 填充已播放荧光条（亮青色 11/3）
    for (int x = sb_x + 2; x < sb_x + 2 + prog_w; x++) {
        for (int y = sb_y + 2; y < sb_y + sb_h - 2; y++) {
            PUT_PIXEL(v, x, y, 9);
        }
    }
    // 播放进度滑块手柄（复古小方块）
    int thumb_x = sb_x + 2 + prog_w;
    if (thumb_x + 6 >= sb_x + sb_w) thumb_x = sb_x + sb_w - 6;
    for (int y = sb_y - 2; y <= sb_y + sb_h + 1; y++) {
        for (int x = thumb_x; x < thumb_x + 6; x++) {
            unsigned char tc = 7;
            if (x == thumb_x || y == sb_y - 2) tc = 15;
            if (x == thumb_x + 5 || y == sb_y + sb_h + 1) tc = 0;
            PUT_PIXEL(v, x, y, tc);
        }
    }
    
    // =========================================================================
    // 3. 经典复古按键组（X: wx+10, Y: wy+120, W: 320, H: 26）
    // =========================================================================
    int disabled = !dev_ok;
    // PREV, PLAY, PAUSE, STOP, NEXT, OPEN
    draw_retro_btn(v, wx + 10,  wy + 120, 36, 22, "|<<", 0, disabled);
    draw_retro_btn(v, wx + 50,  wy + 120, 42, 22, ">",   0, disabled);
    draw_retro_btn(v, wx + 96,  wy + 120, 42, 22, "||",  0, disabled);
    draw_retro_btn(v, wx + 142, wy + 120, 42, 22, "[]",  0, disabled);
    draw_retro_btn(v, wx + 188, wy + 120, 36, 22, ">>|", 0, disabled);
    draw_retro_btn(v, wx + 228, wy + 120, 46, 22, "Open", 0, 0); // 打开文件永远允许操作
    draw_retro_btn(v, wx + 278, wy + 120, 52, 22, "Mute", 0, disabled);
    
    // =========================================================================
    // 4. 音量调节区域与底部状态条（X: wx+10, Y: wy+152, W: 320, H: 24）
    // =========================================================================
    font_draw_text(v, wx + 10, wy + 154, "VOL:", 0);
    int vol_bar_x = wx + 50;
    int vol_bar_y = wy + 156;
    int vol_bar_w = 90;
    int vol_bar_h = 8;
    for (int y = vol_bar_y; y < vol_bar_y + vol_bar_h; y++) {
        for (int x = vol_bar_x; x < vol_bar_x + vol_bar_w; x++) {
            PUT_PIXEL(v, x, y, 0);
        }
    }
    int vol = audio_get_volume(); // 0 ~ 100
    int vol_len = (vol * (vol_bar_w - 4)) / 100;
    for (int y = vol_bar_y + 1; y < vol_bar_y + vol_bar_h - 1; y++) {
        for (int x = vol_bar_x + 2; x < vol_bar_x + 2 + vol_len; x++) {
            PUT_PIXEL(v, x, y, 10); // 荧光绿
        }
    }
    
    // Hardware name on its own full-width row. Long ES1371 codec names wrap
    // instead of spilling outside the player window.
    for (int x = wx + 8; x < wx + ww - 8; x++) {
        PUT_PIXEL(v, x, wy + 172, 0);
        PUT_PIXEL(v, x, wy + 173, 15);
    }
    font_draw_text(v, wx + 10, wy + 178, "DEV:", 0);
    if (dev_ok) {
        draw_wrapped_text(v, wx + 50, wy + 178, (ww - 60) / 8, 2,
                          audio_get_hardware_name(), 0);
    } else {
        font_draw_text(v, wx + 50, wy + 178, "NO AUDIO HARDWARE", 12);
    }
}

void audioplayer_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh) {
    (void)v; (void)ww; (void)wh;
    int dev_ok = audio_has_hardware();
    
    // 1. 进度条点击 Seek
    int sb_x = wx + 10;
    int sb_y = wy + 98;
    int sb_w = 320;
    int sb_h = 14;
    if (mx >= sb_x && mx < sb_x + sb_w && my >= sb_y && my < sb_y + sb_h) {
        if (dev_ok) {
            unsigned int tot = audio_get_duration_sec();
            if (tot > 0) {
                int offset_x = mx - sb_x;
                if (offset_x < 0) offset_x = 0;
                if (offset_x > sb_w) offset_x = sb_w;
                unsigned int target_sec = (unsigned int)(((unsigned int)offset_x * tot) / (unsigned int)sb_w);
                g_drag_seek_target_sec = (int)target_sec;
            }
            is_dragging_seek = 1;
        }
        return;
    }
    
    // 2. 音量条点击调节
    int vol_bar_x = wx + 50;
    int vol_bar_y = wy + 152;
    int vol_bar_w = 90;
    int vol_bar_h = 14;
    if (mx >= vol_bar_x && mx < vol_bar_x + vol_bar_w && my >= vol_bar_y && my < vol_bar_y + vol_bar_h) {
        int vx = mx - vol_bar_x;
        if (vx < 0) vx = 0;
        if (vx > vol_bar_w) vx = vol_bar_w;
        int target_vol = (vx * 100) / vol_bar_w;
        audio_set_volume(target_vol);
        is_dragging_vol = 1;
        return;
    }
    
    // 3. 按钮点击
    int by = wy + 120;
    int bh = 22;
    if (my >= by && my < by + bh) {
        // PREV
        if (mx >= wx + 10 && mx < wx + 46) {
            if (dev_ok) audio_seek_sec(0);
        }
        // PLAY
        else if (mx >= wx + 50 && mx < wx + 92) {
            if (dev_ok) {
                if (audio_get_state() == AUDIO_STATE_PAUSED) {
                    audio_resume();
                } else if (audio_get_state() == AUDIO_STATE_STOPPED) {
                    if (g_last_opened_filepath[0]) {
                        audio_play_file(g_last_opened_filepath);
                    }
                }
            }
        }
        // PAUSE
        else if (mx >= wx + 96 && mx < wx + 138) {
            if (dev_ok) {
                if (audio_get_state() == AUDIO_STATE_PLAYING) {
                    audio_pause();
                } else if (audio_get_state() == AUDIO_STATE_PAUSED) {
                    audio_resume();
                }
            }
        }
        // STOP
        else if (mx >= wx + 142 && mx < wx + 184) {
            if (dev_ok) audio_stop();
        }
        // NEXT
        else if (mx >= wx + 188 && mx < wx + 224) {
            if (dev_ok) audio_seek_sec(0);
        }
        // OPEN
        else if (mx >= wx + 228 && mx < wx + 274) {
            // 打开示例歌曲或唤起文件管理器
            extern int fmv, fmm;
            extern void bring_to_top(int id);
            fmv = 1; fmm = 0;
            bring_to_top(4); // 聚焦文件管理器，方便用户选择音频文件
        }
        // MUTE
        else if (mx >= wx + 278 && mx < wx + 330) {
            if (dev_ok) {
                if (audio_get_volume() > 0) {
                    audio_set_volume(0);
                } else {
                    audio_set_volume(80);
                }
            }
        }
    }
}

int audioplayer_drag(int mx, int my, int wx, int wy, int ww, int wh) {
    (void)my; (void)wy; (void)ww; (void)wh;
    if (is_dragging_seek) {
        int sb_x = wx + 10;
        int sb_w = 320;
        unsigned int tot = audio_get_duration_sec();
        if (tot > 0) {
            int offset_x = mx - sb_x;
            if (offset_x < 0) offset_x = 0;
            if (offset_x > sb_w) offset_x = sb_w;
            unsigned int target_sec = (unsigned int)(((unsigned int)offset_x * tot) / (unsigned int)sb_w);
            g_drag_seek_target_sec = (int)target_sec;
        }
        return 1;
    } else if (is_dragging_vol) {
        int vol_bar_x = wx + 50;
        int vol_bar_w = 90;
        int vx = mx - vol_bar_x;
        if (vx < 0) vx = 0;
        if (vx > vol_bar_w) vx = vol_bar_w;
        int target_vol = (vx * 100) / vol_bar_w;
        audio_set_volume(target_vol);
        return 1;
    }
    return 0;
}

int audioplayer_is_dragging(void) {
    return (is_dragging_seek || is_dragging_vol);
}

void audioplayer_release(void) {
    if (is_dragging_seek) {
        if (g_drag_seek_target_sec >= 0) {
            audio_seek_sec((unsigned int)g_drag_seek_target_sec);
            g_drag_seek_target_sec = -1;
        }
    }
    is_dragging_seek = 0;
    is_dragging_vol = 0;
}
