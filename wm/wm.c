// wm.c - 完整版（含文件管理器）
#include "wm.h"
static int in_ok(int mx, int my, int bx, int by);
static int in_cl(int mx, int my, int cx, int cy);
static int in_mi(int mx, int my, int cx, int cy);
static int rs_dir(int mx, int my, int wx, int wy, int ww, int wh);
#include "font.h"
#include "version.h"
#include "port.h"
#include "calc.h"
#include "cpanel.h"
#include "filemanager.h"
#include "editor.h"
#include "sysinfo.h"
#include "puzzle.h"
#include "imageviewer.h"
#include "audioplayer.h"
#include "audio.h"
#include "wallpaper.h"
#include "speaker.h"
#include "keyboard.h"
#include "mm.h"
#include "power.h"
#include "installer.h"
#include "vfs.h"

// 双击判定的 tick 阈值（100Hz 下 55 ticks = 550ms）
#define DOUBLE_CLICK_TICKS 55

int redraw = 0;
static int need_mr = 0;

// Opened message for visual confirmation when an app is opened
static char opened_msg[48] = "";
static int opened_msg_timer = 0; // ticks

static void show_opened(const char* name)
{
    int i=0;
    while(name[i] && i < (int)sizeof(opened_msg)-1) { opened_msg[i] = name[i]; i++; }
    opened_msg[i] = 0;
    opened_msg_timer = 120; // show for ~120 ticks
    redraw = 1; need_mr = 1;
}

static void my_itoa(unsigned int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0;
    while(n > 0) {
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
static void my_strcat(char* dest, const char* src) {
    while(*dest) dest++;
    while(*src) *dest++ = *src++;
    *dest = '\0';
}
static void my_strcpy(char* dest, const char* src) {
    while(*src) *dest++ = *src++;
    *dest = '\0';
}

static int wx=80, wy=52, ww=420, wh=200;
static int desk_icon_x[11] = {0}, desk_icon_y[11] = {0};
static int icon_dragging = -1;
static int icon_drag_ox = 0, icon_drag_oy = 0;
static int icon_drag_active = 0, icon_drag_bx = 0, icon_drag_by = 0;
static int icon_pressed = -1;
static int icon_press_ox = 0, icon_press_oy = 0;
static int icon_press_moved = 0;
static int menu_open = 0, menu_index = -1;
extern volatile int global_tick;
static int last_click_icon = -1, last_s = -1;
// Track previous click release for double-click timing (separate from visual selection)
static int last_click_icon_prev = -1, last_click_time_prev = 0;

static int wx2=100, wy2=70, ww2=180, wh2=90;
static int ww2_def=180, wh2_def=90;
static int wcx=95, wcy=50, wcw=170, wch=160;
static int wcw_def=170, wch_def=160;
static int cpx=85, cpy=50, cpw=310, cph=210;
static int cpw_def=310, cph_def=210;
static int fmx=85, fmy=45, fmw=440, fmh=260;
static int fmw_def=440, fmh_def=260;
int edv=0, edm=0;
int fmv=0, fmm=0;
static int wv=0, cv=0, clv=0, cpv=0;
static int am=0, cm=0, clm=0, cpm=0;
static int edx=90, edy=60, edw=300, edh=200;
static int edw_def=300, edh_def=200;
static int six=65, siy=35, siw=520, sih=250;
static int siw_def=520, sih_def=250;
static int siv=0, sim=0;
static int pzx=110, pzy=50, pzw=180, pzh=200;
static int pzw_def=180, pzh_def=200;
static int pzv=0, pzm=0;
static int imx=60, imy=35, imw=480, imh=340;
static int imw_def=480, imh_def=340;

static int npx=70, npy=45, npw=340, nph=215;
static int npw_def=340, nph_def=215;
int npv=0, npm=0;

static int insx=80, insy=50, insw=340, insh=220;
static int insw_def=340, insh_def=220;
int insv=0, insm=0;

static int z_order[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
void bring_to_top(int id) {
    int i;
    for(i=0; i<11; i++) if(z_order[i] == id) break;
    if (i < 11) {
        for(int j=i; j<10; j++) z_order[j] = z_order[j+1];
        z_order[10] = id;
    }
}
static int dragging=0, dgo=0, dgoy=0, dgw=0;
static int drag_active=0, drag_bx=0, drag_by=0, drag_bw=0, drag_bh=0;
static int okp=0, okbx=0, okby=0;
static int clp=0, clcx=0, clcy=0, clw=0;
static int mip=0, miw=0;

static int resizing=0, rsdir=0;
static int rsox=0, rsoy=0, rsow=0, rsoh=0;
static int rs_active=0, rs_bx=0, rs_by=0, rs_bw=0, rs_bh=0;
static int power_confirm=0;
static int power_cancel_pressed=0, power_ok_pressed=0;

static void icon(unsigned char* v, int x, int y, unsigned char bg, const char* lb, int sel);

static unsigned char cmos_read(unsigned char a){outb(0x70,a);return inb(0x71);}
static int bcd_to_int(unsigned char b){return(b>>4)*10+(b&0x0F);}
static void get_time(int*h,int*m,int*s){
    int timeout = 10000;
    while ((cmos_read(0x0A) & 0x80) && --timeout);
    unsigned char sec = cmos_read(0x00);
    unsigned char min = cmos_read(0x02);
    unsigned char hr  = cmos_read(0x04);
    unsigned char regb = cmos_read(0x0B);
    if (!(regb & 0x04)) {
        *h = bcd_to_int(hr);
        *m = bcd_to_int(min);
        *s = bcd_to_int(sec);
    } else {
        *h = hr;
        *m = min;
        *s = sec;
    }
}

static void ok_btn(unsigned char* v, int bx, int by, int p)
{
    for(int y=by;y<by+12;y++)for(int x=bx;x<bx+32;x++)PUT_PIXEL(v, x, y, 7);
    if(p){for(int x=bx;x<bx+32;x++)PUT_PIXEL(v, x, by, 0);for(int y=by;y<by+12;y++)PUT_PIXEL(v, bx, y, 0);
           for(int x=bx;x<bx+32;x++)PUT_PIXEL(v, x, (by+11), 15);for(int y=by;y<by+12;y++)PUT_PIXEL(v, bx+31, y, 15);
           font_draw_text(v,bx+9,by+3,"OK",0);}
    else{for(int x=bx;x<bx+32;x++)PUT_PIXEL(v, x, by, 15);for(int y=by;y<by+12;y++)PUT_PIXEL(v, bx, y, 15);
          for(int x=bx;x<bx+32;x++)PUT_PIXEL(v, x, (by+11), 0);for(int y=by;y<by+12;y++)PUT_PIXEL(v, bx+31, y, 0);
          font_draw_text(v,bx+8,by+2,"OK",0);}
}

static void cl_btn(unsigned char* v, int cx, int cy, int p)
{
    for(int y=cy;y<cy+11;y++)for(int x=cx;x<cx+11;x++)PUT_PIXEL(v, x, y, 15);
    for(int i=0;i<11;i++){ PUT_PIXEL(v, cx+i, cy, 0); PUT_PIXEL(v, cx+i, cy+10, 0); PUT_PIXEL(v, cx, cy+i, 0); PUT_PIXEL(v, cx+10, cy+i, 0); }
    if(p){
        for(int y=cy+1;y<cy+10;y++)for(int x=cx+1;x<cx+10;x++)PUT_PIXEL(v, x, y, 0);
    }else{
        for(int i=3;i<8;i++){ PUT_PIXEL(v, cx+i, cy+3, 0); PUT_PIXEL(v, cx+i, cy+7, 0); PUT_PIXEL(v, cx+3, cy+i, 0); PUT_PIXEL(v, cx+7, cy+i, 0); }
    }
}

static void mi_btn(unsigned char* v, int cx, int cy, int p)
{
    for(int y=cy;y<cy+11;y++)for(int x=cx;x<cx+11;x++)PUT_PIXEL(v, x, y, 15);
    for(int i=0;i<11;i++){ PUT_PIXEL(v, cx+i, cy, 0); PUT_PIXEL(v, cx+i, cy+10, 0); PUT_PIXEL(v, cx, cy+i, 0); PUT_PIXEL(v, cx+10, cy+i, 0); }
    if(p){
        for(int y=cy+1;y<cy+10;y++)for(int x=cx+1;x<cx+10;x++)PUT_PIXEL(v, x, y, 0);
    }else{
        for(int i=2;i<9;i++){ PUT_PIXEL(v, cx+i, cy+2, 0); PUT_PIXEL(v, cx+i, cy+8, 0); PUT_PIXEL(v, cx+2, cy+i, 0); PUT_PIXEL(v, cx+8, cy+i, 0); }
        for(int i=3;i<8;i++) PUT_PIXEL(v, cx+i, cy+3, 0);
    }
}

static void window(unsigned char* v, int wx, int wy, const char* t, int ww, int wh, int rs, int active)
{
    // 1px black outline + inner background
    for(int y=wy;y<wy+wh&&y<screen_h;y++)for(int x=wx;x<wx+ww&&x<screen_w;x++) {
        if(y==wy||y==wy+wh-1||x==wx||x==wx+ww-1) PUT_PIXEL(v, x, y, 0);
        else PUT_PIXEL(v, x, y, 7);
    }
    
    // Title bar area (white)
    for(int y=wy+1;y<wy+18&&y<screen_h;y++)for(int x=wx+1;x<wx+ww-1&&x<screen_w;x++) PUT_PIXEL(v, x, y, 15);
    for(int x=wx;x<wx+ww&&x<screen_w;x++) PUT_PIXEL(v, x, wy+18, 0); // Bottom line of title bar
    
    int tw = 0; while(t[tw]) tw++;
    int tx = wx + (ww - tw*8)/2;
    
    if (active) {
        // Draw stripes
        for (int y = wy+3; y <= wy+15; y+=2) {
            for (int x = wx+2; x < wx+ww-2; x++) {
                if ((x >= wx+4 && x <= wx+16) || (x >= wx+ww-18 && x <= wx+ww-6) || (x >= tx-4 && x <= tx+tw*8+4)) continue;
                PUT_PIXEL(v, x, y, 0);
            }
        }
    }
    
    cl_btn(v,wx+4,wy+4,0);
    mi_btn(v,wx+ww-16,wy+4,0);
    
    font_draw_text(v,tx,wy+2,t,0);
    
    if(rs){
        int c=10;
        for(int i=0;i<c;i++)for(int j=0;j<=i;j++){
            int px=wx+ww-c+j,py=wy+wh-c+i;
            if(px<screen_w&&py<screen_h)PUT_PIXEL(v, px, py, 8);
        }
    }
}

static void about_win(unsigned char* v, int wx, int wy)
{
    window(v,wx,wy,"About",ww,wh,0, (z_order[10]==0));
    int lx=wx+(ww-41)/2,ly=wy+25;
    for(int y=ly+3;y<ly+18;y++)for(int x=lx-7;x<lx+48;x++)PUT_PIXEL(v, x, y, 1);
    for(int x=lx-4;x<lx+45;x++)PUT_PIXEL(v, x, (ly+3), 9);
    for(int y=ly;y<ly+12;y++)for(int x=lx;x<lx+12;x++)PUT_PIXEL(v, x, y, (y==ly||x==lx)?12:4);
    for(int y=ly;y<ly+12;y++)for(int x=lx+10;x<lx+22;x++)PUT_PIXEL(v, x, y, (y==ly||x==lx+10)?10:2);
    for(int y=ly+7;y<ly+19;y++)for(int x=lx+19;x<lx+31;x++)PUT_PIXEL(v, x, y, (y==ly+7||x==lx+19)?11:3);
    for(int y=ly+7;y<ly+19;y++)for(int x=lx+29;x<lx+41;x++)PUT_PIXEL(v, x, y, (y==ly+7||x==lx+29)?13:5);
    font_draw_text(v,lx+2,ly+24,"T",1);
    font_draw_text(v,lx+10,ly+24,"B",2);
    font_draw_text(v,lx+18,ly+24,"M",3);
    font_draw_text(v,lx+26,ly+24,"K",5);
    font_draw_text(v, wx+8, wy+60, "Codename Pixel Neko Next", 0);
    font_draw_text(v, wx+8, wy+80, "Version " KERNEL_BUILD, 0);
    font_draw_text(v, wx+8, wy+100, "(C) 2026 Tairitsu_tty / XNZM", 0);
    
    font_draw_text(v, wx+8, wy+120, "CPU:", 0);
    char cpu_disp[64];
    int max_chars = (ww - 56) / 8;
    if (max_chars > 60) max_chars = 60;
    if (max_chars < 1) max_chars = 1;
    int ci = 0;
    while (cpu_model_string[ci] && ci < max_chars) {
        cpu_disp[ci] = cpu_model_string[ci];
        ci++;
    }
    cpu_disp[ci] = '\0';
    font_draw_text(v, wx+48, wy+120, cpu_disp, 0);
    
    char mem_str[64];
    my_strcpy(mem_str, "Total RAM: ");
    char num_buf[16];
    my_itoa(total_ram_mb, num_buf);
    my_strcat(mem_str, num_buf);
    my_strcat(mem_str, " MB / Usable: ");
    my_itoa(usable_ram_mb, num_buf);
    my_strcat(mem_str, num_buf);
    my_strcat(mem_str, " MB");
    
    font_draw_text(v, wx+8, wy+140, mem_str, 0);

    int bx=wx+ww-50,by=wy+160;
    ok_btn(v,bx,by,0);
}

extern unsigned char* lfb;

static void draw_lfb_drag_box(int x, int y, int w, int h)
{
    if (!lfb) return;
    for(int i = x; i < x + w; i += 2) {
        if(i >= 0 && i < screen_w && y >= 0 && y < screen_h) PUT_PIXEL(lfb, i, y, 0);
        if(i >= 0 && i < screen_w && y + h - 1 >= 0 && y + h - 1 < screen_h) PUT_PIXEL(lfb, i, (y + h - 1), 0);
    }
    for(int i = y; i < y + h; i += 2) {
        if(x >= 0 && x < screen_w && i >= 0 && i < screen_h) PUT_PIXEL(lfb, x, i, 0);
        if(x + w - 1 >= 0 && x + w - 1 < screen_w && i >= 0 && i < screen_h) PUT_PIXEL(lfb, (x + w - 1), i, 0);
    }
}

static void erase_lfb_drag_box(const unsigned char* v, int x, int y, int w, int h)
{
    if (!lfb || !v) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;

    copy_rect_to_lfb(v, x, y, w, 1);
    copy_rect_to_lfb(v, x, y + h - 1, w, 1);
    copy_rect_to_lfb(v, x, y, 1, h);
    copy_rect_to_lfb(v, x + w - 1, y, 1, h);
}

static void box(unsigned char* v, int x, int y, int w, int h)
{
    for(int i=x;i<x+w;i+=2){if(i<screen_w&&y<screen_h&&y>=0)PUT_PIXEL(v, i, y, 0);if(i<screen_w&&y+h-1<screen_h&&y+h-1>=0)PUT_PIXEL(v, i, (y+h-1), 0);}
    for(int i=y;i<y+h;i+=2){if(x<screen_w&&i<screen_h&&i>=0)PUT_PIXEL(v, x, i, 0);if(x+w-1<screen_w&&i<screen_h&&i>=0)PUT_PIXEL(v, x+w-1, i, 0);}
}

static void icon(unsigned char* v, int x, int y, unsigned char bg, const char* lb, int sel)
{
    if (sel) {
        for(int i=y-4; i<y+54; i++) for(int j=x-4; j<x+36; j++) PUT_PIXEL(v, j, i, 11);
    }
    
    unsigned char ic[16][16];
    for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=255;
    
    if(bg==1){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=9;
        for(int j=0;j<16;j++) ic[0][j]=11; for(int i=0;i<16;i++) ic[i][0]=11;
        for(int j=0;j<16;j++) ic[15][j]=1; for(int i=0;i<16;i++) ic[i][15]=1;
        for(int i=3;i<7;i++)for(int j=6;j<10;j++) ic[i][j]=15;
        for(int i=8;i<9;i++)for(int j=6;j<10;j++) ic[i][j]=15;
        for(int i=10;i<12;i++)for(int j=7;j<9;j++) ic[i][j]=15;
    }else if(bg==2){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=8;
        for(int i=2;i<14;i++)for(int j=2;j<14;j++) ic[i][j]=15;
        for(int j=2;j<14;j++){ic[2][j]=0;ic[13][j]=0;}
        for(int i=2;i<14;i++){ic[i][2]=0;ic[i][13]=0;}
        ic[8][7]=0;ic[8][8]=0;ic[8][9]=0;
        ic[5][7]=0;ic[6][7]=0;ic[7][7]=0;
        ic[8][7]=4;
    }else if(bg==4){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=0;j<16;j++) ic[0][j]=15; for(int i=0;i<16;i++) ic[i][0]=15;
        for(int j=0;j<16;j++) ic[15][j]=0; for(int i=0;i<16;i++) ic[i][15]=0;
        for(int i=2;i<6;i++)for(int j=2;j<14;j++) ic[i][j]=2;
        for(int j=2;j<14;j++) ic[2][j]=0;
        for(int i=7;i<10;i++)for(int j=2;j<6;j++) ic[i][j]=15;
        for(int i=7;i<10;i++)for(int j=7;j<11;j++) ic[i][j]=15;
        for(int i=11;i<14;i++)for(int j=2;j<6;j++) ic[i][j]=15;
        for(int i=11;i<14;i++)for(int j=7;j<11;j++) ic[i][j]=15;
        for(int i=7;i<14;i++)for(int j=12;j<14;j++) ic[i][j]=4;
        ic[3][8]=0;ic[3][9]=0;ic[3][10]=0;ic[3][11]=0;
    }else if(bg==3){ // Notepad
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int i=1;i<15;i++)for(int j=2;j<13;j++) ic[i][j]=15;
        for(int j=2;j<13;j++) {ic[0][j]=0; ic[15][j]=0;}
        for(int i=0;i<16;i++) {ic[i][1]=0; ic[i][13]=0;}
        for(int i=3;i<13;i+=3)for(int j=4;j<11;j++) ic[i][j]=0;
    }else if(bg==6){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=8;
        for(int j=0;j<16;j++) ic[0][j]=15;
        for(int i=0;i<16;i++) ic[i][0]=15;
        for(int j=0;j<16;j++) ic[15][j]=0;
        for(int i=0;i<16;i++) ic[i][15]=0;
        for(int j=3;j<13;j++) ic[4][j]=15;
        for(int j=3;j<13;j++) ic[8][j]=15;
        for(int j=3;j<13;j++) ic[12][j]=15;
        for(int i=3;i<7;i++)for(int j=7;j<9;j++) ic[i][j]=9;
        ic[5][7]=11;ic[3][7]=11;
    }else if(bg==5){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=0;j<16;j++) ic[0][j]=15;
        for(int i=0;i<16;i++) ic[i][0]=15;
        for(int j=0;j<16;j++) ic[15][j]=0;
        for(int i=0;i<16;i++) ic[i][15]=0;
        for(int i=3;i<13;i++)for(int j=3;j<13;j++) ic[i][j]=6;
        for(int j=3;j<13;j++) ic[3][j]=14;
        for(int i=3;i<5;i++)for(int j=3;j<7;j++) ic[i][j]=14;
    }else if(bg==7){ // SysInfo (Computer)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int i=2;i<10;i++)for(int j=2;j<14;j++) ic[i][j]=15;
        for(int j=2;j<14;j++){ ic[2][j]=0; ic[9][j]=0; }
        for(int i=2;i<10;i++){ ic[i][2]=0; ic[i][13]=0; }
        for(int i=3;i<9;i++)for(int j=3;j<13;j++) ic[i][j]=1; // blue screen
        for(int j=5;j<11;j++) ic[10][j]=8;
        for(int j=3;j<13;j++) ic[11][j]=0;
        for(int j=3;j<13;j++) ic[12][j]=15;
    }else if(bg==8){ // Puzzle (15-Puzzle grid)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=0;j<16;j++){ ic[0][j]=15; ic[15][j]=0; }
        for(int i=0;i<16;i++){ ic[i][0]=15; ic[i][15]=0; }
        for(int i=1;i<15;i++){ ic[i][7]=0; ic[7][i]=0; }
        ic[3][3]=4; ic[3][4]=4; // tile 1
        ic[3][11]=2; ic[3][12]=2; // tile 2
        ic[11][3]=1; ic[11][4]=1; // tile 3
        ic[11][11]=8; ic[12][11]=8; // empty slot
    }else if(bg==9){ // Image Viewer (Photo frame with sun and mountain)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=1;j<15;j++) { ic[1][j]=15; ic[14][j]=0; }
        for(int i=1;i<15;i++) { ic[i][1]=15; ic[i][14]=0; }
        for(int i=2;i<14;i++)for(int j=2;j<14;j++) ic[i][j]=9; // blue sky
        ic[3][10]=14; ic[3][11]=14; ic[4][10]=14; ic[4][11]=14; // sun
        for(int j=2;j<9;j++) ic[13][j]=2;
        for(int j=3;j<8;j++) ic[12][j]=2;
        for(int j=4;j<7;j++) ic[11][j]=2;
        ic[10][5]=10; ic[10][6]=10;
        for(int j=7;j<14;j++) ic[13][j]=10;
        for(int j=8;j<13;j++) ic[12][j]=10;
        for(int j=9;j<12;j++) ic[11][j]=10;
        ic[10][10]=15; ic[10][11]=15; // snow peak
    }else if(bg==10){ // Audio Player (Vinyl record / CD with musical note)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=1;j<15;j++) { ic[1][j]=15; ic[14][j]=0; }
        for(int i=1;i<15;i++) { ic[i][1]=15; ic[i][14]=0; }
        for(int i=2;i<14;i++)for(int j=2;j<14;j++) ic[i][j]=0; // black vinyl
        ic[3][7]=8; ic[3][8]=8; ic[12][7]=8; ic[12][8]=8;
        ic[7][3]=8; ic[8][3]=8; ic[7][12]=8; ic[8][12]=8;
        for(int i=6;i<=9;i++)for(int j=6;j<=9;j++) ic[i][j]=11;
        ic[7][7]=0; ic[8][8]=0;
        ic[3][10]=14; ic[3][11]=14; ic[3][12]=14;
        ic[4][10]=14; ic[4][12]=14;
        ic[5][10]=14; ic[5][12]=14;
        ic[6][9]=14; ic[6][10]=14; ic[6][11]=14; ic[6][12]=14;
    }else if(bg==11){ // Installer (Hard Drive with install arrow)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=255;
        for(int i=6;i<15;i++)for(int j=1;j<15;j++) ic[i][j]=7;
        for(int j=1;j<15;j++){ ic[6][j]=15; ic[14][j]=0; }
        for(int i=6;i<15;i++){ ic[i][1]=15; ic[i][14]=0; }
        for(int j=3;j<13;j++) ic[8][j]=8;
        for(int j=3;j<13;j++) ic[12][j]=0;
        ic[10][11]=10; // green LED
        ic[1][7]=2; ic[1][8]=2;
        ic[2][7]=2; ic[2][8]=2;
        ic[3][7]=2; ic[3][8]=2;
        ic[4][5]=2; ic[4][6]=2; ic[4][7]=2; ic[4][8]=2; ic[4][9]=2; ic[4][10]=2;
        ic[5][6]=2; ic[5][7]=2; ic[5][8]=2; ic[5][9]=2;
        ic[6][7]=2; ic[6][8]=2;
    }
    
    for(int i=0;i<16;i++){
        for(int j=0;j<16;j++){
            unsigned char c = ic[i][j];
            if(c != 255){
                PUT_PIXEL(v, x+j*2, y+i*2, c);
                PUT_PIXEL(v, x+j*2+1, y+i*2, c);
                PUT_PIXEL(v, x+j*2, y+i*2+1, c);
                PUT_PIXEL(v, x+j*2+1, y+i*2+1, c);
            }
        }
    }
    
    if (lb) {
        int tw = 0; while(lb[tw]) tw++;
        int tx = x + 16 - tw*4; // center 32px
        if (sel) {
            for(int yy=y+36; yy<y+48; yy++) for(int xx=tx-2; xx<tx+tw*8+2; xx++) PUT_PIXEL(v, xx, yy, 1);
            font_draw_text(v, tx, y+38, lb, 15);
        } else {
            font_draw_text(v, tx, y+38, lb, 0);
        }
    }
}

static void draw_power_dialog(unsigned char* v)
{
    int dw = 220, dh = 80;
    int dx = (screen_w - dw) / 2;
    int dy = (screen_h - dh) / 2;

    for(int y=dy;y<dy+dh;y++)for(int x=dx;x<dx+dw;x++)PUT_PIXEL(v, x, y, 8);
    for(int y=dy+2;y<dy+dh-2;y++)for(int x=dx+2;x<dx+dw-2;x++)PUT_PIXEL(v, x, y, 7);
    for(int x=dx+2;x<dx+dw-2;x++){PUT_PIXEL(v, x, dy+2, 15);PUT_PIXEL(v, x, dy+dh-3, 0);}
    for(int y=dy+2;y<dy+dh-2;y++){PUT_PIXEL(v, dx+2, y, 15);PUT_PIXEL(v, dx+dw-3, y, 0);}
    if (power_confirm == 1) {
        font_draw_text(v,dx+60,dy+10,"Restart?",0);font_draw_text(v,dx+50,dy+28,"Are you sure?",0);
    } else {
        font_draw_text(v,dx+50,dy+10,"Shutdown?",0);font_draw_text(v,dx+50,dy+28,"Are you sure?",0);
    }
    int bx=dx+20,by=dy+48;
    for(int y=by;y<by+22;y++)for(int x=bx;x<bx+80;x++)PUT_PIXEL(v, x, y, power_cancel_pressed?0:7);
    for(int x=bx;x<bx+80;x++){PUT_PIXEL(v, x, by, power_cancel_pressed?0:15);PUT_PIXEL(v, x, by+21, power_cancel_pressed?15:0);}
    for(int y=by;y<by+22;y++){PUT_PIXEL(v, bx, y, power_cancel_pressed?0:15);PUT_PIXEL(v, bx+79, y, power_cancel_pressed?15:0);}
    font_draw_text(v,bx+16,by+3,"Cancel",power_cancel_pressed?15:0);
    
    bx=dx+110;
    for(int y=by;y<by+22;y++)for(int x=bx;x<bx+80;x++)PUT_PIXEL(v, x, y, power_ok_pressed?4:4);
    for(int x=bx;x<bx+80;x++){PUT_PIXEL(v, x, by, power_ok_pressed?0:12);PUT_PIXEL(v, x, by+21, power_ok_pressed?12:0);}
    for(int y=by;y<by+22;y++){PUT_PIXEL(v, bx, y, power_ok_pressed?0:12);PUT_PIXEL(v, bx+79, y, power_ok_pressed?12:0);}
    font_draw_text(v,bx+32,by+3,"OK",15);
}

static void draw_others(unsigned char* v, int dgw)
{
    for (int i=0; i<11; i++) {
        int id = z_order[i];
        if (id + 1 == dgw) continue;
        if (id == 0 && wv) {
            if (!am) about_win(v, wx, wy);
            else window(v,wx,wy,"About",ww,20,0,(z_order[10]==0));
        }
        if (id == 1 && cv) {
            if (!cm) {
                window(v,wx2,wy2,"Clock",ww2,wh2,1,(z_order[10]==1));
                int h,m,s; get_time(&h,&m,&s); char b[9];
                b[0]='0'+h/10;b[1]='0'+h%10;b[2]=':';b[3]='0'+m/10;b[4]='0'+m%10;b[5]=':';b[6]='0'+s/10;b[7]='0'+s%10;b[8]=0;
                font_draw_text(v, wx2+ww2/2-32, wy2+wh2/2-8, b, 0);
            } else window(v,wx2,wy2,"Clock",ww2,20,0,(z_order[10]==1));
        }
        if (id == 2 && clv) {
            if (!clm) {
                window(v,wcx,wcy,"Calculator",wcw,wch,2,(z_order[10]==2));
                calc_draw(v, wcx, wcy, wcw, wch);
            } else window(v,wcx,wcy,"Calculator",wcw,20,0,(z_order[10]==2));
        }
        if (id == 3 && cpv) {
            if (!cpm) {
                window(v,cpx,cpy,"Control Panel",cpw,cph,3,(z_order[10]==3));
                cpanel_draw(v, cpx, cpy, cpw, cph);
            } else window(v,cpx,cpy,"Control Panel",cpw,20,0,(z_order[10]==3));
        }
        if (id == 4 && fmv) {
            if (!fmm) {
                window(v,fmx,fmy,"File Manager",fmw,fmh,4,(z_order[10]==4));
                fm_draw(v, fmx, fmy, fmw, fmh);
            } else window(v,fmx,fmy,"File Manager",fmw,20,0,(z_order[10]==4));
        }
        if (id == 5 && edv) {
            if (!edm) {
                window(v,edx,edy,"Notepad",edw,edh,5,(z_order[10]==5));
                editor_draw(v, edx, edy+20, edw, edh-20);
            } else window(v,edx,edy,"Notepad",edw,20,0,(z_order[10]==5));
        }
        if (id == 6 && siv) {
            if (!sim) {
                window(v,six,siy,"System Information",siw,sih,6,(z_order[10]==6));
                sysinfo_draw(v, six, siy, siw, sih);
            } else window(v,six,siy,"System Information",siw,20,0,(z_order[10]==6));
        }
        if (id == 7 && pzv) {
            if (!pzm) {
                window(v,pzx,pzy,"15-Puzzle",pzw,pzh,7,(z_order[10]==7));
                puzzle_draw(v, pzx, pzy, pzw, pzh);
            } else window(v,pzx,pzy,"15-Puzzle",pzw,20,0,(z_order[10]==7));
        }
        if (id == 8 && imv) {
            if (!imm) {
                window(v,imx,imy,imageviewer_get_title(),imw,imh,1,(z_order[10]==8));
                imageviewer_draw(v, imx, imy, imw, imh);
            } else window(v,imx,imy,imageviewer_get_title(),imw,20,0,(z_order[10]==8));
        }
        if (id == 9 && npv) {
            if (!npm) {
                window(v,npx,npy,audioplayer_get_title(),npw,nph,0,(z_order[10]==9));
                audioplayer_draw(v, npx, npy, npw, nph);
            } else window(v,npx,npy,audioplayer_get_title(),npw,20,0,(z_order[10]==9));
        }
        if (id == 10 && insv) {
            if (!insm) {
                window(v,insx,insy,"Install OS to Disk",insw,insh,0,(z_order[10]==10));
                installer_draw(v, insx, insy, insw, insh);
            } else window(v,insx,insy,"Install OS to Disk",insw,20,0,(z_order[10]==10));
        }
    }
}

static void draw_all(unsigned char* v)
{
    draw_others(v, 0);
}

static void draw_menu_popup(unsigned char* v, int x, int y, int w, int h)
{
    for(int py = y; py < y + h && py < screen_h; py++) {
        for(int px = x; px < x + w && px < screen_w; px++) {
            PUT_PIXEL(v, px, py, 15);
        }
    }
    for(int px = x; px < x + w && px < screen_w; px++) {
        PUT_PIXEL(v, px, y, 0);
        PUT_PIXEL(v, px, y + h - 1, 0);
    }
    for(int py = y; py < y + h && py < screen_h; py++) {
        PUT_PIXEL(v, x, py, 0);
        PUT_PIXEL(v, x + w - 1, py, 0);
    }
    for(int py = y + 2; py < y + h + 2 && py < screen_h; py++) {
        if (x + w < screen_w) PUT_PIXEL(v, x + w, py, 0);
        if (x + w + 1 < screen_w) PUT_PIXEL(v, x + w + 1, py, 0);
    }
    for(int px = x + 2; px < x + w + 2 && px < screen_w; px++) {
        if (y + h < screen_h) PUT_PIXEL(v, px, y + h, 0);
        if (y + h + 1 < screen_h) PUT_PIXEL(v, px, y + h + 1, 0);
    }
}

static void draw_menubar(unsigned char* v)
{
    for(int y=0; y<20; y++) for(int x=0; x<screen_w; x++) PUT_PIXEL(v, x, y, 15);
    for(int x=0; x<screen_w; x++) PUT_PIXEL(v, x, 20, 0);
    for(int y=4; y<16; y++) for(int x=10; x<22; x++) PUT_PIXEL(v, x, y, (y/2)%6+1);
    
    if (menu_open) {
        int mx = 30 + menu_index * 40;
        int mw = (menu_index == 3) ? 64 : 40; 
        if (menu_index != 4) {
            for(int y=2; y<18; y++) for(int x=mx-4; x<mx+mw; x++) PUT_PIXEL(v, x, y, 1);
        }
        font_draw_text(v, 30, 2, "File", menu_index==0?15:0);
        font_draw_text(v, 70, 2, "Edit", menu_index==1?15:0);
        font_draw_text(v, 110, 2, "View", menu_index==2?15:0);
        font_draw_text(v, 150, 2, "Special", menu_index==3?15:0);
        
        if (menu_index == 4) { // Apple Logo
            draw_menu_popup(v, 4, 20, 172, 4*18+4);
            font_draw_text(v, 10, 24, "About Pixel Neko", 0);
            font_draw_text(v, 10, 24+18, "System Information", 0);
            font_draw_text(v, 10, 24+36, "Restart Computer", 0);
            font_draw_text(v, 10, 24+54, "Shut Down Computer", 0);
        } else if (menu_index == 0) { // File
            if (z_order[10] == 5 && edv && !edm) { // Notepad active
                draw_menu_popup(v, 30, 20, 130, 4*18+4);
                font_draw_text(v, 38, 24, "New", 0);
                font_draw_text(v, 38, 24+18, "Save", 0);
                font_draw_text(v, 38, 24+36, "Save As...", 0);
                font_draw_text(v, 38, 24+54, "Quit (Not Save)", 0);
            } else {
                draw_menu_popup(v, 30, 20, 110, 1*18+4);
                font_draw_text(v, 38, 24, "(Empty)", 8);
            }
        } else if (menu_index == 1) { // Edit
            draw_menu_popup(v, 66, 20, 120, 6*18+4);
            font_draw_text(v, 74, 24, "Undo", 8);
            int ed_active = (z_order[10] == 5 && edv && !edm);
            int has_sel = (ed_active && editor_has_selection());
            font_draw_text(v, 74, 24+18, "Cut", has_sel ? 0 : 8);
            font_draw_text(v, 74, 24+36, "Copy", has_sel ? 0 : 8);
            extern char g_clipboard[4096];
            font_draw_text(v, 74, 24+54, "Paste", (ed_active && g_clipboard[0]) ? 0 : 8);
            font_draw_text(v, 74, 24+72, "Select All", ed_active ? 0 : 8);
            font_draw_text(v, 74, 24+90, "Clear", has_sel ? 0 : 8);
        } else if (menu_index == 2) { // View
            draw_menu_popup(v, 106, 20, 160, 1*18+4);
            font_draw_text(v, 114, 24, "Clean Up Desktop", 0);
        } else if (menu_index == 3) { // Special
            draw_menu_popup(v, 146, 20, 172, 6*18+4);
            font_draw_text(v, 154, 24, "About Pixel Neko", 0);
            font_draw_text(v, 154, 24+18, "System Information", 0);
            font_draw_text(v, 154, 24+36, "15-Puzzle", 0);
            font_draw_text(v, 154, 24+54, "Empty Trash", 0);
            font_draw_text(v, 154, 24+72, "Restart Computer", 0);
            font_draw_text(v, 154, 24+90, "Shut Down Computer", 0);
        }
    } else {
        font_draw_text(v, 30, 2, "File", 0);
        font_draw_text(v, 70, 2, "Edit", 0);
        font_draw_text(v, 110, 2, "View", 0);
        font_draw_text(v, 150, 2, "Special", 0);
    }
    
    int h,m,s; get_time(&h,&m,&s); char b[9];
    b[0]='0'+h/10;b[1]='0'+h%10;b[2]=':';b[3]='0'+m/10;b[4]='0'+m%10;b[5]=':';b[6]='0'+s/10;b[7]='0'+s%10;b[8]=0;
    font_draw_text(v, screen_w - 75, 2, b, 0);

    if (opened_msg[0]) {
        int ox = screen_w / 2 - 40;
        if (ox < 10) ox = 10;
        font_draw_text(v, ox, 22, opened_msg, 0);
    }
}

extern int g_desktop_pattern;
static const unsigned char g_patterns[6][8] = {
    { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 }, // 0: Stipple 50%
    { 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 }, // 1: Diagonal
    { 0x80, 0x00, 0x08, 0x00, 0x80, 0x00, 0x08, 0x00 }, // 2: Micro Dots
    { 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33 }, // 3: Brick Tile
    { 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00 }, // 4: Slate Gray
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  // 5: Solid Gray
};

void wm_init(void){
    wx=80;wy=52;ww=420;wh=200;wx2=100;wy2=70;ww2=180;wh2=90;ww2_def=180;wh2_def=90;
    wcx=95;wcy=50;wcw=170;wch=160;wcw_def=170;wch_def=160;
    cpx=85;cpy=50;cpw=310;cph=210;cpw_def=310;cph_def=210;
    fmx=85;fmy=45;fmw=440;fmh=260;fmw_def=440;fmh_def=260;
    edx=90;edy=60;edw=300;edh=200;edw_def=300;edh_def=200;
    six=65;siy=40;siw=520;sih=250;siw_def=520;sih_def=250;
    npx=70;npy=45;npw=340;nph=215;npw_def=340;nph_def=215;
    pzx=110;pzy=50;pzw=180;pzh=200;pzw_def=180;pzh_def=200;
    for(int i=0;i<11;i++){
        int col = i / 6;
        int row = i % 6;
        desk_icon_x[i] = screen_w - 60 - col * 64;
        desk_icon_y[i] = 30 + row * 52;
    }
    calc_init();cpanel_init();fm_init();sysinfo_init();puzzle_init();imageviewer_init();audioplayer_init();installer_init();redraw=1;
}

void wm_draw(unsigned char* v){
    if(redraw){
        extern void mouse_invalidate(void);
        mouse_invalidate();
        if (wallpaper_is_active()) {
            wallpaper_render(v, screen_w, screen_h, screen_bpp, screen_pitch);
        } else {
            if (g_desktop_pattern < 0 || g_desktop_pattern > 5) g_desktop_pattern = 0;
            if (g_desktop_pattern == 5) {
                for (int y = 0; y < screen_h; y++) {
                    for (int x = 0; x < screen_w; x++) {
                        PUT_PIXEL(v, x, y, 7);
                    }
                }
            } else {
                for (int y = 0; y < screen_h; y++) {
                    unsigned char row_pat = g_patterns[g_desktop_pattern][y & 7];
                    for (int x = 0; x < screen_w; x++) {
                        unsigned char c = (row_pat & (0x80 >> (x & 7))) ? 8 : 7;
                        PUT_PIXEL(v, x, y, c);
                    }
                }
            }
        }
        int num_icons = 11;
        for (int i=0; i<num_icons; i++) {
            if (desk_icon_x[i] == 0 || desk_icon_x[i] > screen_w - 32) desk_icon_x[i] = screen_w - 60 - (i/6)*64;
            if (desk_icon_y[i] == 0 || desk_icon_y[i] > screen_h - 32) desk_icon_y[i] = 30 + (i%6)*52;
        }
        icon(v,desk_icon_x[0],desk_icon_y[0],1,"About", last_click_icon == 0);
        icon(v,desk_icon_x[1],desk_icon_y[1],2,"Clock", last_click_icon == 1);
        icon(v,desk_icon_x[2],desk_icon_y[2],4,"Calc", last_click_icon == 2);
        icon(v,desk_icon_x[3],desk_icon_y[3],6,"CPanel", last_click_icon == 3);
        icon(v,desk_icon_x[4],desk_icon_y[4],5,"Files", last_click_icon == 4);
        icon(v,desk_icon_x[5],desk_icon_y[5],3,"Notepad", last_click_icon == 5);
        icon(v,desk_icon_x[6],desk_icon_y[6],7,"SysInfo", last_click_icon == 6);
        icon(v,desk_icon_x[7],desk_icon_y[7],8,"Puzzle", last_click_icon == 7);
        icon(v,desk_icon_x[8],desk_icon_y[8],9,"Viewer", last_click_icon == 8);
        icon(v,desk_icon_x[9],desk_icon_y[9],10,"Player", last_click_icon == 9);
        icon(v,desk_icon_x[10],desk_icon_y[10],11,"Install", last_click_icon == 10);
        draw_all(v);
        draw_menubar(v);
        if(power_confirm) {  draw_power_dialog(v); }
    }
    redraw=0;
}


static int in_icon(int mx, int my, int i) {
    return (mx >= desk_icon_x[i] - 4 && mx < desk_icon_x[i] + 36 &&
            my >= desk_icon_y[i] - 4 && my < desk_icon_y[i] + 52);
}

static void open_icon_app(int clicked_icon) {
    if(clicked_icon == 0) { wv = 1; am = 0; show_opened("About"); }
    if(clicked_icon == 1) { cv = 1; cm = 0; ww2 = ww2_def; wh2 = wh2_def; show_opened("Clock"); }
    if(clicked_icon == 2) { clv = 1; clm = 0; wcw = wcw_def; wch = wch_def; calc_init(); show_opened("Calc"); }
    if(clicked_icon == 3) { cpv = 1; cpm = 0; cpw = cpw_def; cph = cph_def; cpanel_init(); show_opened("CPanel"); }
    if(clicked_icon == 4) { fmv = 1; fmm = 0; fmw = fmw_def; fmh = fmh_def; fm_init(); show_opened("Files"); }
    if(clicked_icon == 5) {
        if (!edv) { extern void editor_init(void); editor_init(); }
        edv = 1; edm = 0; edw = edw_def; edh = edh_def; show_opened("Notepad");
    }
    if(clicked_icon == 6) {
        siv = 1; sim = 0; siw = siw_def; sih = sih_def; sysinfo_init(); show_opened("SysInfo");
    }
    if(clicked_icon == 7) {
        pzv = 1; pzm = 0; pzw = pzw_def; pzh = pzh_def; puzzle_init(); show_opened("Puzzle");
    }
    if(clicked_icon == 8) {
        imv = 1; imm = 0; imw = imw_def; imh = imh_def; imageviewer_show_sample(); show_opened("Viewer");
    }
    if(clicked_icon == 9) {
        npv = 1; npm = 0; npw = npw_def; nph = nph_def; audioplayer_init(); show_opened("Player");
    }
    if(clicked_icon == 10) {
        insv = 1; insm = 0; insw = insw_def; insh = insh_def; installer_init(); show_opened("Install");
    }
    bring_to_top(clicked_icon);
    last_click_icon = -1;
    last_click_icon_prev = -1;
    last_click_time_prev = 0;
    icon_pressed = -1;
    icon_dragging = -1;
    redraw = 1;
    need_mr = 1;
}

static int in_ok(int mx, int my, int bx, int by) { return (mx>=bx && mx<bx+45 && my>=by && my<by+20); }
static int in_cl(int mx, int my, int cx, int cy) { return (mx>=cx && mx<cx+12 && my>=cy && my<cy+12); }
static int in_mi(int mx, int my, int cx, int cy) { return (mx>=cx && mx<cx+12 && my>=cy && my<cy+12); }
static int rs_dir(int mx, int my, int wx, int wy, int ww, int wh) {
    if(mx>=wx+ww-10 && mx<wx+ww && my>=wy+wh-10 && my<wy+wh) return 1;
    return 0;
}
int wm_is_dragging(void){return dragging;}
int wm_need_mouse_reset(void){int r=need_mr;need_mr=0;return r;}

void wm_update(unsigned char* v, int mx, int my, int mb, int mb_press, int mb_release)
{
    static int last_processed_tick = 0;
    int dt = global_tick - last_processed_tick;
    if (dt < 0) dt = 0;
    if (dt > 0) {
        last_processed_tick = global_tick;
        if (opened_msg_timer > 0) {
            opened_msg_timer -= dt;
            if (opened_msg_timer <= 0) {
                opened_msg_timer = 0;
                opened_msg[0] = 0;
                redraw = 1;
                need_mr = 1;
            }
        }
    }
    
    // Poll RTC once every 100 ticks (1 Hz / 1 second) to prevent ISA bus stalls
    static int last_rtc_tick = 0;
    if (global_tick - last_rtc_tick >= 100 || last_s == -1) {
        last_rtc_tick = global_tick;
        int h, m, s;
        get_time(&h, &m, &s);
        if (s != last_s) {
            last_s = s;
            if (!dragging) redraw = 1;
        }
    }

    if(power_confirm){
        int dw = 220, dh = 80;
        int dx = (screen_w - dw) / 2;
        int dy = (screen_h - dh) / 2;
        int cbx = dx + 20, oby = dy + 48, obx = dx + 110;

        if(mb_press && mx>=cbx && mx<cbx+80 && my>=oby && my<oby+22){power_cancel_pressed=1;need_mr=1;}
        if(mb_release && power_cancel_pressed){power_cancel_pressed=0;
            if(mx>=cbx && mx<cbx+80 && my>=oby && my<oby+22){power_confirm=0;redraw=1;}else{redraw=1;}}
        if(mb_press && mx>=obx && mx<obx+80 && my>=oby && my<oby+22){power_ok_pressed=1;need_mr=1;}
        if(mb_release && power_ok_pressed){power_ok_pressed=0;
            if(mx>=obx && mx<obx+80 && my>=oby && my<oby+22){
                if(power_confirm==1) {
                    sys_reboot();
                } else {
                    sys_shutdown();
                }
            }else{redraw=1;}}
        if(redraw) { need_mr=1; }
        return;
    }

    // Menu bar interaction
    if (menu_open && mb_press) {
        if (menu_index == 4) { // Apple Logo
            if (mx >= 4 && mx < 4 + 172 && my >= 20 && my < 20 + 4*18 + 4) {
                int item = (my - 24) / 18;
                if (item >= 0 && item < 4) {
                    if (item == 0) { wv = 1; am = 0; bring_to_top(0); }
                    if (item == 1) { siv = 1; sim = 0; sysinfo_init(); bring_to_top(6); }
                    if (item == 2) { power_confirm = 1; }
                    if (item == 3) { power_confirm = 2; }
                }
            }
        } else if (menu_index == 0) { // File
            if (z_order[10] == 5 && edv && !edm && mx >= 30 && mx < 30 + 130 && my >= 20 && my < 20 + 4*18 + 4) {
                int item = (my - 24) / 18;
                extern void editor_init(void);
                extern void editor_save(void);
                extern void editor_save_as(void);
                if (item == 0) { editor_init(); redraw = 1; }
                else if (item == 1) { editor_save(); redraw = 1; }
                else if (item == 2) { editor_save_as(); redraw = 1; }
                else if (item == 3) { edv = 0; editor_init(); redraw = 1; }
            }
        } else if (menu_index == 1) { // Edit
            if (z_order[10] == 5 && edv && !edm && mx >= 66 && mx < 66 + 120 && my >= 20 && my < 20 + 6*18 + 4) {
                int item = (my - 24) / 18;
                extern void editor_cut(void);
                extern void editor_copy(void);
                extern void editor_paste(void);
                extern void editor_select_all(void);
                extern void editor_clear(void);
                if (item == 1) { editor_cut(); redraw = 1; }
                else if (item == 2) { editor_copy(); redraw = 1; }
                else if (item == 3) { editor_paste(); redraw = 1; }
                else if (item == 4) { editor_select_all(); redraw = 1; }
                else if (item == 5) { editor_clear(); redraw = 1; }
            }
        } else if (menu_index == 2) { // View
            if (mx >= 106 && mx < 106 + 160 && my >= 20 && my < 20 + 1*18 + 4) {
                for (int i = 0; i < 10; i++) {
                    int col = i / 6;
                    int row = i % 6;
                    desk_icon_x[i] = screen_w - 60 - col * 64;
                    desk_icon_y[i] = 30 + row * 52;
                }
                show_opened("Desktop Cleaned");
            }
        } else if (menu_index == 3) { // Special
            if (mx >= 146 && mx < 146 + 172 && my >= 20 && my < 20 + 6*18 + 4) {
                int item = (my - 24) / 18;
                if (item >= 0 && item < 6) {
                    if (item == 0) { wv = 1; am = 0; bring_to_top(0); }
                    if (item == 1) { siv = 1; sim = 0; sysinfo_init(); bring_to_top(6); }
                    if (item == 2) { pzv = 1; pzm = 0; puzzle_init(); bring_to_top(7); }
                    if (item == 3) { speaker_beep(); show_opened("Trash Emptied"); }
                    if (item == 4) { power_confirm = 1; }
                    if (item == 5) { power_confirm = 2; }
                }
            }
        }
        menu_open = 0;
        redraw = 1;
        need_mr = 1;
        return;
    }
    
    if(mb_press && my < 20) { 
        if (mx >= 4 && mx < 24) { menu_open = (menu_open && menu_index == 4) ? 0 : 1; menu_index = 4; redraw = 1; need_mr = 1; }
        else if (mx >= 30 && mx < 62) { menu_open = (menu_open && menu_index == 0) ? 0 : 1; menu_index = 0; redraw = 1; need_mr = 1; }
        else if (mx >= 70 && mx < 102) { menu_open = (menu_open && menu_index == 1) ? 0 : 1; menu_index = 1; redraw = 1; need_mr = 1; }
        else if (mx >= 110 && mx < 142) { menu_open = (menu_open && menu_index == 2) ? 0 : 1; menu_index = 2; redraw = 1; need_mr = 1; }
        else if (mx >= 150 && mx < 206) { menu_open = (menu_open && menu_index == 3) ? 0 : 1; menu_index = 3; redraw = 1; need_mr = 1; }
        else { menu_open = 0; redraw = 1; }
        return;
    }
    if ((mb & 1)) {
        if (z_order[10] == 4 && fmv && !fmm) {
            if (fm_drag(mx, my, fmx, fmy, fmw, fmh)) {
                redraw = 1;
            }
        }
        if (z_order[10] == 5 && edv && !edm && mx >= edx && mx < edx+edw && my >= edy+20 && my < edy+edh) {
            if (editor_drag(mx, my, edx, edy+20, edw, edh-20)) {
                redraw = 1;
            }
        }
        if (z_order[10] == 8 && imv && !imm && mx >= imx && mx < imx+imw && my >= imy+20 && my < imy+imh) {
            imageviewer_drag(mx, my, imx, imy, imw, imh);
        }
        if (!dragging && z_order[10] == 9 && npv && !npm) {
            if (audioplayer_drag(mx, my, npx, npy, npw, nph)) {
                redraw = 1;
            }
        }
    }
    if (mb_release) {
        editor_release();
        imageviewer_release();
        audioplayer_release();
        fm_release();
    }
    
    // Keyboard input to active window
    int key = keyboard_get_key();
    while (key != 0) {
        if (z_order[10] == 4 && fmv && !fmm) {
            fm_handle_key(key);
            redraw = 1;
        }
        if (z_order[10] == 5 && edv && !edm) {
            editor_handle_key(key);
            redraw = 1;
        }
        if (z_order[10] == 8 && imv && !imm) {
            imageviewer_handle_key(key);
            redraw = 1;
        }
        key = keyboard_get_key();
    }
    
    // Desktop icon press: record press and wait to see if it becomes a drag or a click
    if (mb_press && !dragging && !resizing && !power_confirm && !mip && !clp && !okp && icon_dragging == -1) {
        int total_icons = 11;
        int clicked_icon = -1;
        for (int i = total_icons - 1; i >= 0; i--) {
            if (in_icon(mx, my, i)) {
                clicked_icon = i;
                break;
            }
        }
        if (clicked_icon != -1) {
            // Check double-click on PRESS (instantaneous response, like native Windows)
            if (last_click_icon_prev == clicked_icon && (global_tick - last_click_time_prev) < DOUBLE_CLICK_TICKS) {
                open_icon_app(clicked_icon);
                return;
            }
            icon_pressed = clicked_icon;
            icon_press_ox = mx - desk_icon_x[clicked_icon];
            icon_press_oy = my - desk_icon_y[clicked_icon];
            icon_press_moved = 0;
            // Immediate visual feedback on press
            last_click_icon = clicked_icon;
            redraw = 1;
            need_mr = 1;
        }
    }
    
    if (icon_dragging != -1) {
        if (mb) {
            int nix = mx - icon_drag_ox;
            int niy = my - icon_drag_oy;
            if (nix < 0) nix = 0;
            if (nix + 36 > screen_w) nix = screen_w - 36;
            if (niy < 20) niy = 20;
            if (niy + 48 > screen_h) niy = screen_h - 48;

            if (!icon_drag_active) {
                icon_drag_active = 1;
                icon_drag_bx = nix;
                icon_drag_by = niy;
                draw_lfb_drag_box(icon_drag_bx, icon_drag_by, 36, 48);
            } else if (nix != icon_drag_bx || niy != icon_drag_by) {
                erase_lfb_drag_box(v, icon_drag_bx, icon_drag_by, 36, 48);
                icon_drag_bx = nix;
                icon_drag_by = niy;
                draw_lfb_drag_box(icon_drag_bx, icon_drag_by, 36, 48);
            }
        } else {
            if (icon_drag_active) {
                erase_lfb_drag_box(v, icon_drag_bx, icon_drag_by, 36, 48);
                icon_drag_active = 0;
            }
            desk_icon_x[icon_dragging] = icon_drag_bx;
            desk_icon_y[icon_dragging] = icon_drag_by;
            icon_dragging = -1;
            redraw = 1;
            need_mr = 1;
        }
        return;
    }

    if (icon_pressed != -1) {
        if (mb) {
            int dx = mx - (desk_icon_x[icon_pressed] + icon_press_ox);
            int dy = my - (desk_icon_y[icon_pressed] + icon_press_oy);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > 8 || dy > 8) {
                icon_dragging = icon_pressed;
                icon_drag_ox = icon_press_ox;
                icon_drag_oy = icon_press_oy;
                icon_drag_bx = desk_icon_x[icon_pressed];
                icon_drag_by = desk_icon_y[icon_pressed];
                icon_drag_active = 0;
                icon_pressed = -1;
                icon_press_moved = 1;
                need_mr = 1;
                return;
            }
        } else {
            if (!icon_press_moved && in_icon(mx, my, icon_pressed)) {
                if (last_click_icon_prev == icon_pressed && (global_tick - last_click_time_prev) < DOUBLE_CLICK_TICKS) {
                    open_icon_app(icon_pressed);
                    return;
                } else {
                    last_click_icon_prev = icon_pressed;
                    last_click_time_prev = global_tick;
                    redraw = 1;
                }
            }
            icon_pressed = -1;
            need_mr = 1;
        }
    }

    if (resizing) {
        if (mb) {
            int nw = 0, nh = 0, rx = 0, ry = 0;
            if (dgw == 2) { nw = mx - wcx; nh = my - wcy; if(nw<100)nw=100; if(nh<100)nh=100; rx = wcx; ry = wcy; }
            else if (dgw == 3) { nw = mx - cpx; nh = my - cpy; if(nw<200)nw=200; if(nh<150)nh=150; rx = cpx; ry = cpy; }
            else if (dgw == 4) { nw = mx - fmx; nh = my - fmy; if(nw<200)nw=200; if(nh<150)nh=150; rx = fmx; ry = fmy; }
            else if (dgw == 5) { nw = mx - edx; nh = my - edy; if(nw<200)nw=200; if(nh<150)nh=150; rx = edx; ry = edy; }
            else if (dgw == 9) { nw = mx - imx; nh = my - imy; if(nw<220)nw=220; if(nh<160)nh=160; rx = imx; ry = imy; }
            
            if (rx + nw > screen_w) nw = screen_w - rx;
            if (ry + nh > screen_h) nh = screen_h - ry;

            if (!rs_active) {
                rs_active = 1;
                rs_bx = rx; rs_by = ry; rs_bw = nw; rs_bh = nh;
                draw_lfb_drag_box(rs_bx, rs_by, rs_bw, rs_bh);
            } else if (nw != rs_bw || nh != rs_bh) {
                erase_lfb_drag_box(v, rs_bx, rs_by, rs_bw, rs_bh);
                rs_bw = nw; rs_bh = nh;
                draw_lfb_drag_box(rs_bx, rs_by, rs_bw, rs_bh);
            }
        } else {
            if (rs_active) {
                erase_lfb_drag_box(v, rs_bx, rs_by, rs_bw, rs_bh);
                rs_active = 0;
            }
            if (dgw == 2) { wcw = rs_bw; wch = rs_bh; }
            else if (dgw == 3) { cpw = rs_bw; cph = rs_bh; }
            else if (dgw == 4) { fmw = rs_bw; fmh = rs_bh; }
            else if (dgw == 5) { edw = rs_bw; edh = rs_bh; }
            else if (dgw == 9) { imw = rs_bw; imh = rs_bh; }
            resizing = 0;
            dgw = 0;
            redraw = 1;
            need_mr = 1;
        }
        return;
    }
    
    if (mb_press && !dragging && !okp && !clp && !mip && !resizing) {
        int hit_window = 0;
        for (int i=10; i>=0; i--) {
            int id = z_order[i];
            if (id == 10 && insv && !insm && mx>=insx && mx<insx+insw && my>=insy && my<insy+insh) { hit_window=1; bring_to_top(10); installer_click(v, mx, my, insx, insy, insw, insh); redraw=1; break; }
            if (id == 9 && npv && !npm && mx>=npx && mx<npx+npw && my>=npy && my<npy+nph) { hit_window=1; bring_to_top(9); audioplayer_click(v, mx, my, npx, npy, npw, nph); redraw=1; break; }
            if (id == 8 && imv && !imm && rs_dir(mx,my,imx,imy,imw,imh)){resizing=1;dgw=9;rs_active=0;bring_to_top(8);break;}
            if (id == 8 && imv && !imm && mx>=imx && mx<imx+imw && my>=imy && my<imy+imh) { hit_window=1; bring_to_top(8); imageviewer_click(v, mx, my, imx, imy, imw, imh); redraw=1; break; }
            if (id == 7 && pzv && !pzm && mx>=pzx && mx<pzx+pzw && my>=pzy && my<pzy+pzh) { hit_window=1; bring_to_top(7); puzzle_click(mx, my, pzx, pzy, pzw, pzh); redraw=1; break; }
            if (id == 6 && siv && !sim && mx>=six && mx<six+siw && my>=siy && my<siy+sih) { hit_window=1; bring_to_top(6); sysinfo_click(mx, my, six, siy, siw, sih); redraw=1; break; }
            if (id == 5 && edv && !edm && rs_dir(mx,my,edx,edy,edw,edh)){resizing=1;dgw=5;rs_active=0;bring_to_top(5);break;}
            if (id == 5 && edv && !edm && mx>=edx && mx<edx+edw && my>=edy && my<edy+edh) { hit_window=1; bring_to_top(5); editor_click(mx, my, edx, edy+20, edw, edh-20); redraw=1; break; }
            if (id == 4 && fmv && !fmm && rs_dir(mx,my,fmx,fmy,fmw,fmh)){resizing=1;dgw=4;rs_active=0;bring_to_top(4);break;}
            if (id == 4 && fmv && !fmm && mx>=fmx && mx<fmx+fmw && my>=fmy && my<fmy+fmh) { hit_window=1; bring_to_top(4); fm_click(v, mx, my, mb, fmx, fmy, fmw, fmh); redraw=1; break; }
            if (id == 3 && cpv && !cpm && rs_dir(mx,my,cpx,cpy,cpw,cph)){resizing=1;dgw=3;rs_active=0;bring_to_top(3);break;}
            if (id == 3 && cpv && !cpm && mx>=cpx && mx<cpx+cpw && my>=cpy && my<cpy+cph) { hit_window=1; bring_to_top(3); redraw=1; break; }
            if (id == 2 && clv && !clm && rs_dir(mx,my,wcx,wcy,wcw,wch)){resizing=1;dgw=2;rs_active=0;bring_to_top(2);break;}
            if (id == 2 && clv && !clm && mx>=wcx && mx<wcx+wcw && my>=wcy && my<wcy+wch) { hit_window=1; bring_to_top(2); redraw=1; break; }
            if (id == 1 && cv && !cm && mx>=wx2 && mx<wx2+ww2 && my>=wy2 && my<wy2+wh2) { hit_window=1; bring_to_top(1); redraw=1; break; }
            if (id == 0 && wv && !am && mx>=wx && mx<wx+ww && my>=wy && my<wy+wh) { hit_window=1; bring_to_top(0); redraw=1; break; }
        }
        if (!hit_window && icon_pressed == -1) {
            if (last_click_icon != -1 || last_click_icon_prev != -1) {
                last_click_icon = -1;
                last_click_icon_prev = -1;
                redraw = 1;
            }
        }
    }

    int bx=wx+ww-50,by=wy+160;
    if(mb_press&&wv&&!am&&in_ok(mx,my,bx,by)){okp=1;okbx=bx;okby=by;ok_btn(v,bx,by,1);need_mr=1;}
    if(mb_release&&okp){okp=0;ok_btn(v,okbx,okby,0);if(in_ok(mx,my,okbx,okby)){wv=0;am=0;redraw=1;}else{redraw=1;}}

    if(mb_press&&!dragging&&!okp&&!clp&&!mip&&!resizing){
        if(wv){int cx=wx+4,cy=wy+3;if(in_cl(mx,my,cx,cy)){clp=1;clw=1;clcx=cx;clcy=cy;cl_btn(v,cx,cy,1);need_mr=1;}}
        if(!clp&&cv){int cx2=wx2+4,cy2=wy2+3;if(in_cl(mx,my,cx2,cy2)){clp=1;clw=2;clcx=cx2;clcy=cy2;cl_btn(v,cx2,cy2,1);need_mr=1;}}
        if(!clp&&clv){int cx3=wcx+4,cy3=wcy+3;if(in_cl(mx,my,cx3,cy3)){clp=1;clw=3;clcx=cx3;clcy=cy3;cl_btn(v,cx3,cy3,1);need_mr=1;}}
        if(!clp&&cpv){int cx4=cpx+4,cy4=cpy+3;if(in_cl(mx,my,cx4,cy4)){clp=1;clw=4;clcx=cx4;clcy=cy4;cl_btn(v,cx4,cy4,1);need_mr=1;}}
        if(!clp&&fmv){int cx5=fmx+4,cy5=fmy+3;if(in_cl(mx,my,cx5,cy5)){clp=1;clw=5;clcx=cx5;clcy=cy5;cl_btn(v,cx5,cy5,1);need_mr=1;}}
        if(!clp&&edv){int cx6=edx+4,cy6=edy+3;if(in_cl(mx,my,cx6,cy6)){clp=1;clw=6;clcx=cx6;clcy=cy6;cl_btn(v,cx6,cy6,1);need_mr=1;}}
        if(!clp&&siv){int cx7=six+4,cy7=siy+3;if(in_cl(mx,my,cx7,cy7)){clp=1;clw=7;clcx=cx7;clcy=cy7;cl_btn(v,cx7,cy7,1);need_mr=1;}}
        if(!clp&&pzv){int cx8=pzx+4,cy8=pzy+3;if(in_cl(mx,my,cx8,cy8)){clp=1;clw=8;clcx=cx8;clcy=cy8;cl_btn(v,cx8,cy8,1);need_mr=1;}}
        if(!clp&&imv){int cx9=imx+4,cy9=imy+3;if(in_cl(mx,my,cx9,cy9)){clp=1;clw=9;clcx=cx9;clcy=cy9;cl_btn(v,cx9,cy9,1);need_mr=1;}}
        if(!clp&&npv){int cx10=npx+4,cy10=npy+3;if(in_cl(mx,my,cx10,cy10)){clp=1;clw=10;clcx=cx10;clcy=cy10;cl_btn(v,cx10,cy10,1);need_mr=1;}}
        if(!clp&&insv){int cx11=insx+4,cy11=insy+3;if(in_cl(mx,my,cx11,cy11)){clp=1;clw=11;clcx=cx11;clcy=cy11;cl_btn(v,cx11,cy11,1);need_mr=1;}}
    }
    if(mb_release&&clp){cl_btn(v,clcx,clcy,0);if(in_cl(mx,my,clcx,clcy)){
        if(clw==1){wv=0;am=0;}else if(clw==2){cv=0;cm=0;}else if(clw==3){clv=0;clm=0;}else if(clw==4){cpv=0;cpm=0;}else if(clw==5){fmv=0;fmm=0;}else if(clw==6){edv=0;edm=0;}else if(clw==7){siv=0;sim=0;}else if(clw==8){pzv=0;pzm=0;}else if(clw==9){imv=0;imm=0;}else if(clw==10){npv=0;npm=0;audio_stop();}else if(clw==11){insv=0;insm=0;}redraw=1;}else{redraw=1;}clp=0;clw=0;}

    if(mb_press&&!dragging&&!okp&&!clp&&!mip&&!resizing){
        if(wv){int mx2=wx+ww-16,my2=wy+4;if(in_mi(mx,my,mx2,my2)){mip=1;miw=1;mi_btn(v,mx2,my2,1);need_mr=1;}}
        if(!mip&&cv){int mx3=wx2+ww2-16,my3=wy2+4;if(in_mi(mx,my,mx3,my3)){mip=1;miw=2;mi_btn(v,mx3,my3,1);need_mr=1;}}
        if(!mip&&clv){int mx4=wcx+wcw-16,my4=wcy+4;if(in_mi(mx,my,mx4,my4)){mip=1;miw=3;mi_btn(v,mx4,my4,1);need_mr=1;}}
        if(!mip&&cpv){int mx5=cpx+cpw-16,my5=cpy+4;if(in_mi(mx,my,mx5,my5)){mip=1;miw=4;mi_btn(v,mx5,my5,1);need_mr=1;}}
        if(!mip&&fmv){int mx6=fmx+fmw-16,my6=fmy+4;if(in_mi(mx,my,mx6,my6)){mip=1;miw=5;mi_btn(v,mx6,my6,1);need_mr=1;}}
        if(!mip&&edv){int mx7=edx+edw-16,my7=edy+4;if(in_mi(mx,my,mx7,my7)){mip=1;miw=6;mi_btn(v,mx7,my7,1);need_mr=1;}}
        if(!mip&&siv){int mx8=six+siw-16,my8=siy+4;if(in_mi(mx,my,mx8,my8)){mip=1;miw=7;mi_btn(v,mx8,my8,1);need_mr=1;}}
        if(!mip&&pzv){int mx9=pzx+pzw-16,my9=pzy+4;if(in_mi(mx,my,mx9,my9)){mip=1;miw=8;mi_btn(v,mx9,my9,1);need_mr=1;}}
        if(!mip&&imv){int mx10=imx+imw-16,my10=imy+4;if(in_mi(mx,my,mx10,my10)){mip=1;miw=9;mi_btn(v,mx10,my10,1);need_mr=1;}}
        if(!mip&&npv){int mx11=npx+npw-16,my11=npy+4;if(in_mi(mx,my,mx11,my11)){mip=1;miw=10;mi_btn(v,mx11,my11,1);need_mr=1;}}
        if(!mip&&insv){int mx12=insx+insw-16,my12=insy+4;if(in_mi(mx,my,mx12,my12)){mip=1;miw=11;mi_btn(v,mx12,my12,1);need_mr=1;}}
    }
    if(mb_release&&mip){if(in_mi(mx,my,(miw==1?wx+ww-16:miw==2?wx2+ww2-16:miw==3?wcx+wcw-16:miw==4?cpx+cpw-16:miw==5?fmx+fmw-16:miw==6?edx+edw-16:miw==7?six+siw-16:miw==8?pzx+pzw-16:miw==9?imx+imw-16:miw==10?npx+npw-16:insx+insw-16),(miw==1?wy+4:miw==2?wy2+4:miw==3?wcy+4:miw==4?cpy+4:miw==5?fmy+4:miw==6?edy+4:miw==7?siy+4:miw==8?pzy+4:miw==9?imy+4:miw==10?npy+4:insy+4))){
        if(miw==1){am=!am;}else if(miw==2){cm=!cm;}
        else if(miw==3){clm=!clm;}
        else if(miw==4){cpm=!cpm;}
        else if(miw==5){fmm=!fmm;}
        else if(miw==6){edm=!edm;}
        else if(miw==7){sim=!sim;}
        else if(miw==8){pzm=!pzm;}
        else if(miw==9){imm=!imm;}else if(miw==10){npm=!npm;}else if(miw==11){insm=!insm;}
        redraw=1;}else{redraw=1;}
        mip=0;
    }

    if(mb_press&&cv&&!cm&&!dragging&&!resizing){
        int d=rs_dir(mx,my,wx2,wy2,ww2,wh2);
        if(d){resizing=1;rsdir=d;rsox=wx2;rsoy=wy2;rsow=ww2;rsoh=wh2;}
    }
    if(mb&1&&resizing){
        int nx=wx2,ny=wy2,nw=ww2,nh=wh2;
        if(rsdir==1||rsdir==3){nx=mx;nw=rsox+rsow-mx;}if(rsdir==2||rsdir==4){nw=mx-wx2;}
        if(rsdir==1||rsdir==2){ny=my;nh=rsoy+rsoh-my;}if(rsdir==3||rsdir==4){nh=my-wy2;}
        if(nw<60){if(rsdir==1||rsdir==3)nx=rsox+rsow-60;nw=60;}
        if(nh<40){if(rsdir==1||rsdir==2)ny=rsoy+rsoh-40;nh=40;}
        if(nw!=ww2||nh!=wh2||nx!=wx2||ny!=wy2){wx2=nx;wy2=ny;ww2=nw;wh2=nh;redraw=1;}
    }
    if(mb_release&&resizing){resizing=0;redraw=1;}

    if(mb_press&&clv&&!clm&&!clp&&!mip){calc_click(v,mx,my,wcx,wcy,wcw,wch);redraw=1;}
    if(mb_press&&cpv&&!cpm&&!clp&&!mip){cpanel_click(v,mx,my,cpx,cpy,cpw,cph);redraw=1;}
    if(mb_press&&siv&&!sim&&!clp&&!mip){sysinfo_click(mx,my,six,siy,siw,sih);redraw=1;}
    if(mb_press&&pzv&&!pzm&&!clp&&!mip){puzzle_click(mx,my,pzx,pzy,pzw,pzh);redraw=1;}

    if(mb&1&&!okp&&!clp&&!mip&&!resizing){
        if(!dragging){
            if(wv&&mx>=wx&&mx<wx+ww&&my>=wy&&my<wy+12){bring_to_top(0);dragging=1;dgw=1;dgo=mx-wx;dgoy=my-wy;}
            else if(cv&&mx>=wx2&&mx<wx2+ww2&&my>=wy2&&my<wy2+12){bring_to_top(1);dragging=1;dgw=2;dgo=mx-wx2;dgoy=my-wy2;}
            else if(clv&&mx>=wcx&&mx<wcx+wcw&&my>=wcy&&my<wcy+12){bring_to_top(2);dragging=1;dgw=3;dgo=mx-wcx;dgoy=my-wcy;}
            else if(cpv&&mx>=cpx&&mx<cpx+cpw&&my>=cpy&&my<cpy+12){bring_to_top(3);dragging=1;dgw=4;dgo=mx-cpx;dgoy=my-cpy;}
            else if(fmv&&mx>=fmx&&mx<fmx+fmw&&my>=fmy&&my<fmy+12){bring_to_top(4);dragging=1;dgw=5;dgo=mx-fmx;dgoy=my-fmy;}
            else if(edv&&mx>=edx&&mx<edx+edw&&my>=edy&&my<edy+12){bring_to_top(5);dragging=1;dgw=6;dgo=mx-edx;dgoy=my-edy;}
            else if(siv&&mx>=six&&mx<six+siw&&my>=siy&&my<siy+12){bring_to_top(6);dragging=1;dgw=7;dgo=mx-six;dgoy=my-siy;}
            else if(pzv&&mx>=pzx&&mx<pzx+pzw&&my>=pzy&&my<pzy+12){bring_to_top(7);dragging=1;dgw=8;dgo=mx-pzx;dgoy=my-pzy;}
            else if(imv&&mx>=imx&&mx<imx+imw&&my>=imy&&my<imy+12){bring_to_top(8);dragging=1;dgw=9;dgo=mx-imx;dgoy=my-imy;}
            else if(npv&&mx>=npx&&mx<npx+npw&&my>=npy&&my<npy+12){bring_to_top(9);dragging=1;dgw=10;dgo=mx-npx;dgoy=my-npy;}
            else if(insv&&mx>=insx&&mx<insx+insw&&my>=insy&&my<insy+12){bring_to_top(10);dragging=1;dgw=11;dgo=mx-insx;dgoy=my-insy;}
        }
        if(dragging){
            int nwx=mx-dgo,nwy=my-dgoy;
            int nww=dgw==1?ww:dgw==2?ww2:dgw==3?wcw:dgw==4?cpw:dgw==5?fmw:dgw==6?edw:dgw==7?siw:dgw==8?pzw:dgw==9?imw:dgw==10?npw:insw;
            int nwh=dgw==1?wh:dgw==2?wh2:dgw==3?wch:dgw==4?cph:dgw==5?fmh:dgw==6?edh:dgw==7?sih:dgw==8?pzh:dgw==9?imh:dgw==10?nph:insh;
            if(dgw==1&&am) nwh=19; 
            if(dgw==2&&cm) nwh=19; 
            if(dgw==3&&clm) nwh=19; 
            if(dgw==4&&cpm) nwh=19; 
            if(dgw==5&&fmm) nwh=19; 
            if(dgw==6&&edm) nwh=19;
            if(dgw==7&&sim) nwh=19;
            if(dgw==8&&pzm) nwh=19;
            if(dgw==9&&imm) nwh=19;
            if(dgw==10&&npm) nwh=19;
            if(dgw==11&&insm) nwh=19;
            if(nwx<0) nwx=0;
            if(nwy<0) nwy=0;
            if(nwx+nww>screen_w) nwx=screen_w-nww;
            if(nwy+nwh>screen_h) nwy=screen_h-nwh;

            if(!drag_active){
                drag_active = 1;
                drag_bx = nwx; drag_by = nwy; drag_bw = nww; drag_bh = nwh;
                draw_lfb_drag_box(drag_bx, drag_by, drag_bw, drag_bh);
            } else if(nwx != drag_bx || nwy != drag_by || nww != drag_bw || nwh != drag_bh) {
                erase_lfb_drag_box(v, drag_bx, drag_by, drag_bw, drag_bh);
                drag_bx = nwx; drag_by = nwy; drag_bw = nww; drag_bh = nwh;
                draw_lfb_drag_box(drag_bx, drag_by, drag_bw, drag_bh);
            }
        }
    }else{
        if(dragging){
            if(drag_active){
                erase_lfb_drag_box(v, drag_bx, drag_by, drag_bw, drag_bh);
                drag_active = 0;
            }
            int*cwx=dgw==1?&wx:dgw==2?&wx2:dgw==3?&wcx:dgw==4?&cpx:dgw==5?&fmx:dgw==6?&edx:dgw==7?&six:dgw==8?&pzx:dgw==9?&imx:dgw==10?&npx:&insx;
            int*cwy=dgw==1?&wy:dgw==2?&wy2:dgw==3?&wcy:dgw==4?&cpy:dgw==5?&fmy:dgw==6?&edy:dgw==7?&siy:dgw==8?&pzy:dgw==9?&imy:dgw==10?&npy:&insy;
            *cwx = drag_bx;
            *cwy = drag_by;
            dragging = 0;
            dgw = 0;
            redraw = 1;
            need_mr = 1;
        }
    }
    if(redraw) need_mr=1;
}
