#include "cpanel.h"
#include "font.h"
#include "mouse.h"
#include "wm.h"
#include "port.h"

static int slider_val = 2;

int revert_timer = 0;
static int prev_w = 800, prev_h = 600, prev_bpp = 16;
extern void set_resolution(int w, int h, int bpp);
extern volatile int global_tick;

static unsigned char cmos_get_sec(void) {
    int timeout = 10000;
    outb(0x70, 0x0A);
    while ((inb(0x71) & 0x80) && --timeout);
    outb(0x70, 0x00);
    return inb(0x71);
}

void check_revert_timer(void)
{
    if (revert_timer <= 0) return;
    static unsigned char last_cmos_sec = 0xFF;
    unsigned char cur_sec = cmos_get_sec();
    if (last_cmos_sec == 0xFF) last_cmos_sec = cur_sec;

    if (cur_sec != last_cmos_sec) {
        last_cmos_sec = cur_sec;
        if (revert_timer > 0) {
            revert_timer--;
            extern int redraw;
            redraw = 1;
            if (revert_timer == 0) {
                set_resolution(prev_w, prev_h, prev_bpp);
                redraw = 1;
            }
        }
    }
}

void cpanel_init(void)
{
    slider_val = mouse_get_speed();
}

static void draw_btn(unsigned char* v, int bx, int by, int bw, int bh, const char* text)
{
    for(int y=by; y<by+bh; y++) for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, y, 7);
    for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, by, 15);
    for(int y=by; y<by+bh; y++) PUT_PIXEL(v, bx, y, 15);
    for(int x=bx; x<bx+bw; x++) PUT_PIXEL(v, x, by+bh-1, 0);
    for(int y=by; y<by+bh; y++) PUT_PIXEL(v, bx+bw-1, y, 0);
    font_draw_text(v, bx+4, by+(bh/2)-4, text, 0);
}

void cpanel_draw(unsigned char* v, int wx, int wy, int ww, int wh)
{
    (void)ww; (void)wh;
    font_draw_text(v, wx+8, wy+20, "Mouse Speed", 0);
    int tx = wx+20, ty = wy+35;
    for(int y=ty; y<ty+8; y++) for(int x=tx; x<tx+100; x++) PUT_PIXEL(v, x, y, 7);
    for(int x=tx; x<tx+100; x++) PUT_PIXEL(v, x, ty, 0);
    for(int x=tx; x<tx+100; x++) PUT_PIXEL(v, x, (ty+7), 0);
    for(int y=ty; y<ty+8; y++) {PUT_PIXEL(v, tx, y, 0); PUT_PIXEL(v, tx+99, y, 0);}
    for(int i=0;i<5;i++){int sx=tx+i*20+8;for(int y=ty-2;y<ty+10;y++)for(int x=sx;x<sx+4;x++)PUT_PIXEL(v, x, y, 0);}
    int sx=tx+slider_val*20-4, sy=ty-3;
    for(int y=sy;y<sy+14;y++)for(int x=sx;x<sx+12;x++)PUT_PIXEL(v, x, y, 9);
    for(int x=sx;x<sx+12;x++)PUT_PIXEL(v, x, sy, 11);
    for(int y=sy;y<sy+14;y++)PUT_PIXEL(v, sx, y, 11);
    for(int x=sx;x<sx+12;x++)PUT_PIXEL(v, x, (sy+13), 1);
    for(int y=sy;y<sy+14;y++)PUT_PIXEL(v, sx+11, y, 1);

    font_draw_text(v, wx+10, wy+60, "Display Settings", 0);
    draw_btn(v, wx+10, wy+80, 115, 24, "320x200 8"); draw_btn(v, wx+130, wy+80, 115, 24, "640x480 16");
    draw_btn(v, wx+10, wy+110, 115, 24, "800x600 16"); draw_btn(v, wx+130, wy+110, 115, 24, "1024x768 16");
    
    if (revert_timer > 0) {
        int sec = revert_timer;
        char buf[30] = "Revert?   s";
        buf[8] = '0' + (sec / 10);
        buf[9] = '0' + (sec % 10);
        font_draw_text(v, wx+10, wy+146, buf, 4);
        draw_btn(v, wx+130, wy+140, 115, 24, "YES");
    } else {
        font_draw_text(v, wx+10, wy+146, "Live Mode: Session settings", 8);
    }
}

void cpanel_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh)
{
    (void)v; (void)ww; (void)wh;
    int tx=wx+20, ty=wy+35;
    if(mx>=tx && mx<tx+100 && my>=ty && my<ty+16) {
        int val = (mx - tx) / 20;
        if(val < 0) val = 0;
        if(val > 4) val = 4;
        slider_val = val;
        mouse_set_speed(val);
        extern int redraw;
        redraw = 1;
    }
    
    if (revert_timer > 0) {
        if(mx>=wx+130 && mx<wx+245 && my>=wy+140 && my<wy+164) {
            revert_timer = 0;
            extern int redraw;
            redraw = 1;
        }
        return;
    }

    if(mx>=wx+10 && mx<wx+125 && my>=wy+80 && my<wy+104) { prev_w = screen_w; prev_h = screen_h; prev_bpp = screen_bpp; set_resolution(320, 200, 8); revert_timer = 15; }
    if(mx>=wx+130 && mx<wx+245 && my>=wy+80 && my<wy+104) { prev_w = screen_w; prev_h = screen_h; prev_bpp = screen_bpp; set_resolution(640, 480, 16); revert_timer = 15; }
    if(mx>=wx+10 && mx<wx+125 && my>=wy+110 && my<wy+134) { prev_w = screen_w; prev_h = screen_h; prev_bpp = screen_bpp; set_resolution(800, 600, 16); revert_timer = 15; }
    if(mx>=wx+130 && mx<wx+245 && my>=wy+110 && my<wy+134) { prev_w = screen_w; prev_h = screen_h; prev_bpp = screen_bpp; set_resolution(1024, 768, 16); revert_timer = 15; }
}