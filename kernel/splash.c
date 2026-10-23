#include "splash.h"
#include "wm.h"
#include "font.h"
#include "port.h"
#include "timer.h"
#include "speaker.h"
#include "cpu.h"
#include "mm.h"
#include "fat.h"
#include "vfs.h"
#include "mouse.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

// Check if user pressed a key (Esc/Space/Enter) or clicked mouse to skip animation
static int splash_poll_skip(void)
{
    while (inb(0x64) & 0x01) {
        unsigned char st = inb(0x64);
        unsigned char data = inb(0x60);
        if (st & 0x20) {
            // Mouse event
            if (data & 0x07) return 1;
        } else {
            // Keyboard event: Esc (0x01), Space (0x39), Enter (0x1C)
            if (data == 0x01 || data == 0x39 || data == 0x1C) return 1;
        }
    }
    return 0;
}

static void draw_filled_box(unsigned char* v, int bx, int by, int bw, int bh, unsigned char color)
{
    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            PUT_PIXEL(v, x, y, color);
        }
    }
}

static void draw_3d_frame(unsigned char* v, int bx, int by, int bw, int bh, int recessed)
{
    unsigned char top_left = recessed ? 8 : 15;
    unsigned char bot_right = recessed ? 15 : 8;

    for (int x = bx; x < bx + bw; x++) {
        PUT_PIXEL(v, x, by, top_left);
        PUT_PIXEL(v, x, by + bh - 1, bot_right);
    }
    for (int y = by; y < by + bh; y++) {
        PUT_PIXEL(v, bx, y, top_left);
        PUT_PIXEL(v, bx + bw - 1, y, bot_right);
    }
}

static void draw_retro_dither_bg(unsigned char* v)
{
    // Classic 50% Macintosh stipple checkerboard pattern
    for (int y = 0; y < screen_h; y++) {
        for (int x = 0; x < screen_w; x++) {
            unsigned char c = ((x ^ y) & 1) ? 15 : 7;
            PUT_PIXEL(v, x, y, c);
        }
    }
}

// ----------------------------------------------------------------------------
// Phase 1: Happy Neko (Compact Mac CRT with smiling pixel Neko)
// ----------------------------------------------------------------------------

static void draw_happy_neko(unsigned char* v, int cx, int cy)
{
    int w = 64, h = 72;
    int x0 = cx - w / 2;
    int y0 = cy - h / 2;

    // Drop shadow
    draw_filled_box(v, x0 + 3, y0 + 3, w, h, 8);

    // Monitor case
    draw_filled_box(v, x0, y0, w, h, 7);

    // 1px black outline
    for (int x = x0; x < x0 + w; x++) {
        PUT_PIXEL(v, x, y0, 0);
        PUT_PIXEL(v, x, y0 + h - 1, 0);
    }
    for (int y = y0; y < y0 + h; y++) {
        PUT_PIXEL(v, x0, y, 0);
        PUT_PIXEL(v, x0 + w - 1, y, 0);
    }

    // Top & left 3D highlights
    for (int x = x0 + 1; x < x0 + w - 1; x++) PUT_PIXEL(v, x, y0 + 1, 15);
    for (int y = y0 + 1; y < y0 + h - 1; y++) PUT_PIXEL(v, x0 + 1, y, 15);

    // Recessed top handle
    draw_filled_box(v, x0 + 20, y0 + 4, 24, 3, 8);
    for (int x = x0 + 20; x < x0 + 44; x++) PUT_PIXEL(v, x, y0 + 7, 15);

    // CRT Screen Bezel (Recessed)
    int sx = x0 + 8, sy = y0 + 12, sw = 48, sh = 42;
    draw_3d_frame(v, sx, sy, sw, sh, 1);
    draw_filled_box(v, sx + 1, sy + 1, sw - 2, sh - 2, 15);

    // CRT Glass Inner border
    for (int x = sx + 2; x < sx + sw - 2; x++) {
        PUT_PIXEL(v, x, sy + 2, 7);
        PUT_PIXEL(v, x, sy + sh - 3, 7);
    }
    for (int y = sy + 2; y < sy + sh - 2; y++) {
        PUT_PIXEL(v, sx + 2, y, 7);
        PUT_PIXEL(v, sx + sw - 3, y, 7);
    }

    // Inside CRT: Draw Smiling Happy Neko (Handcrafted 24x22 pixel art)
    static const char* neko_art[20] = {
        "        X     X         ",
        "       XX     XX        ",
        "      X.X     X.X       ",
        "     X...XXXXX...X      ",
        "    X.............X     ",
        "   X...............X    ",
        "   X...............X    ",
        "  X..XX.........XX..X   ",
        "  X.X  X.......X  X.X   ",
        "  X.X  X.......X  X.X   ",
        "  X..XX.........XX..X   ",
        "  X........X........X   ",
        " =X.......XXX.......X=  ",
        "==X......X.X.X......X== ",
        "  X.......XXX.......X   ",
        "   X...............X    ",
        "    X...XX...XX...X     ",
        "     XXX  XXX  XXX      ",
        "      XX   X   XX       ",
        "                        "
    };

    int nx = sx + (sw - 24) / 2;
    int ny = sy + (sh - 20) / 2 - 1;
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 24; c++) {
            char ch = neko_art[r][c];
            if (ch == 'X') {
                PUT_PIXEL(v, nx + c, ny + r, 0); // Black outlines
            } else if (ch == '.') {
                PUT_PIXEL(v, nx + c, ny + r, 13); // Soft pink inside ears/cheeks
            } else if (ch == '=') {
                PUT_PIXEL(v, nx + c, ny + r, 8); // Whisker tips
            }
        }
    }

    // Floppy disk slot on case
    draw_filled_box(v, x0 + 12, y0 + 60, 32, 2, 0);
    for (int x = x0 + 12; x < x0 + 44; x++) PUT_PIXEL(v, x, y0 + 62, 15);

    // Activity LED
    PUT_PIXEL(v, x0 + 48, y0 + 60, 10); // Green LED
    PUT_PIXEL(v, x0 + 49, y0 + 60, 10);
    PUT_PIXEL(v, x0 + 48, y0 + 61, 2);
    PUT_PIXEL(v, x0 + 49, y0 + 61, 2);

    // Paw badge on bottom-left
    PUT_PIXEL(v, x0 + 8, y0 + 60, 8);
    PUT_PIXEL(v, x0 + 9, y0 + 60, 8);
    PUT_PIXEL(v, x0 + 8, y0 + 61, 15);
}

// ----------------------------------------------------------------------------
// Phase 2: Extension Parade Icons (8 retro 18x18 hardware icons)
// ----------------------------------------------------------------------------

static void draw_extension_icon(unsigned char* v, int ix, int iy, int icon_id)
{
    // Draw icon background container (18x18, beveled)
    draw_filled_box(v, ix, iy, 18, 18, 7);
    draw_3d_frame(v, ix, iy, 18, 18, 0);

    switch (icon_id) {
        case 0: // CPU: Ceramic package with gold pins and silicon die
            for (int i = 3; i < 15; i++) {
                PUT_PIXEL(v, ix + i, iy + 2, 14); // Gold pins
                PUT_PIXEL(v, ix + i, iy + 15, 14);
                PUT_PIXEL(v, ix + 2, iy + i, 14);
                PUT_PIXEL(v, ix + 15, iy + i, 14);
            }
            draw_filled_box(v, ix + 3, iy + 3, 12, 12, 8); // Dark ceramic
            draw_filled_box(v, ix + 6, iy + 6, 6, 6, 1);   // Blue die
            PUT_PIXEL(v, ix + 7, iy + 7, 15);
            PUT_PIXEL(v, ix + 8, iy + 7, 15);
            break;

        case 1: // RAM: Green DIMM PCB with DRAM chips and gold edge contacts
            draw_filled_box(v, ix + 2, iy + 4, 14, 10, 2); // Green PCB
            for (int x = ix + 3; x < ix + 15; x += 2) {
                PUT_PIXEL(v, x, iy + 14, 14); // Gold contacts
            }
            draw_filled_box(v, ix + 4, iy + 6, 2, 4, 0); // Black chips
            draw_filled_box(v, ix + 7, iy + 6, 2, 4, 0);
            draw_filled_box(v, ix + 10, iy + 6, 2, 4, 0);
            draw_filled_box(v, ix + 13, iy + 6, 2, 4, 0);
            break;

        case 2: // Display / VGA: Mini CRT monitor with blue screen
            draw_filled_box(v, ix + 3, iy + 3, 12, 9, 8);
            draw_filled_box(v, ix + 4, iy + 4, 10, 7, 9); // Blue screen
            draw_filled_box(v, ix + 7, iy + 12, 4, 2, 8); // Stand
            draw_filled_box(v, ix + 5, iy + 14, 8, 2, 8); // Base
            PUT_PIXEL(v, ix + 13, iy + 11, 10); // Green power LED
            break;

        case 3: // Keyboard: Beige keyboard with keycaps
            draw_filled_box(v, ix + 2, iy + 6, 14, 8, 7);
            for (int y = iy + 7; y < iy + 12; y += 2) {
                for (int x = ix + 3; x < ix + 14; x += 2) {
                    PUT_PIXEL(v, x, y, 0); // Keycap
                }
            }
            // Spacebar
            for (int x = ix + 6; x < ix + 12; x++) PUT_PIXEL(v, x, iy + 12, 0);
            break;

        case 4: // Mouse: Two-button classic mouse
            draw_filled_box(v, ix + 5, iy + 3, 8, 12, 15);
            for (int x = ix + 5; x < ix + 13; x++) {
                PUT_PIXEL(v, x, iy + 3, 0);
                PUT_PIXEL(v, x, iy + 14, 0);
            }
            for (int y = iy + 3; y < iy + 15; y++) {
                PUT_PIXEL(v, ix + 5, y, 0);
                PUT_PIXEL(v, ix + 12, y, 0);
            }
            for (int x = ix + 5; x < ix + 13; x++) PUT_PIXEL(v, x, iy + 7, 0); // Button split
            PUT_PIXEL(v, ix + 8, iy + 7, 0);
            PUT_PIXEL(v, ix + 8, iy + 2, 8); // Cable
            PUT_PIXEL(v, ix + 7, iy + 1, 8);
            break;

        case 5: // Hard Disk: Metal enclosure with spinning platter and LED
            draw_filled_box(v, ix + 3, iy + 2, 12, 14, 8);
            draw_filled_box(v, ix + 4, iy + 3, 10, 12, 7);
            draw_filled_box(v, ix + 6, iy + 5, 6, 6, 15); // Platter
            draw_filled_box(v, ix + 8, iy + 7, 2, 2, 0);  // Spindle
            PUT_PIXEL(v, ix + 4, iy + 13, 12); // Amber read LED
            PUT_PIXEL(v, ix + 5, iy + 13, 10); // Green write LED
            break;

        case 6: // Folder / Storage Volume: Classic yellow folder
            draw_filled_box(v, ix + 3, iy + 5, 5, 2, 14); // Tab
            draw_filled_box(v, ix + 3, iy + 6, 12, 9, 14); // Folder body
            for (int x = ix + 3; x < ix + 15; x++) PUT_PIXEL(v, x, iy + 14, 6);
            for (int y = iy + 6; y < iy + 15; y++) PUT_PIXEL(v, ix + 14, y, 6);
            // White document peeking
            draw_filled_box(v, ix + 6, iy + 3, 6, 4, 15);
            break;

        case 7: // Desktop Window: Window with blue titlebar and closebox
            draw_filled_box(v, ix + 2, iy + 2, 14, 14, 15);
            draw_filled_box(v, ix + 2, iy + 2, 14, 4, 1); // Blue titlebar
            PUT_PIXEL(v, ix + 3, iy + 3, 15); // Closebox
            draw_3d_frame(v, ix + 2, iy + 2, 14, 14, 0);
            break;

        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// Phase 2: Main Welcome Dialog + Mac OS 9 Barber-Pole Striped Progress Bar
// ----------------------------------------------------------------------------

typedef struct {
    const char* status;
    const char* detail;
    int icon_id;
    int target_percent;
    int min_ms;
    void (*exec)(void);
} boot_step_t;

static void run_vfs_init(void) {
    vfs_init();
}

void splash_run(void)
{
    unsigned char* v = backbuffer;
    int skip = 0;

    // ------------------------------------------------------------------------
    // Part 1: Happy Neko (Classic Mac SE + smiling cat + vintage boot chord)
    // ------------------------------------------------------------------------
    draw_retro_dither_bg(v);
    draw_happy_neko(v, screen_w / 2, screen_h / 2 - 15);
    copy_fullscreen_to_lfb(v);

    // Play retro vintage boot chord
    speaker_boot_chord();

    // Hold Happy Neko for ~600ms unless skipped
    for (int t = 0; t < 60; t++) {
        timer_update();
        if (splash_poll_skip()) { skip = 1; break; }
        sleep_ms(10);
    }

    // ------------------------------------------------------------------------
    // Part 2: Welcome to Pixel Neko (Mac OS 9 + Win 3.1 hybrid splash)
    // ------------------------------------------------------------------------
    int win_w = 440;
    int win_h = 230;
    if (win_w > screen_w - 20) win_w = screen_w - 20;
    if (win_h > screen_h - 20) win_h = screen_h - 20;

    int win_x = (screen_w - win_w) / 2;
    int win_y = (screen_h - win_h) / 2 - 20;

    // Reset screen to solid platinum gray
    draw_filled_box(v, 0, 0, screen_w, screen_h, 7);

    // Welcome Window Drop Shadow
    draw_filled_box(v, win_x + 3, win_y + 3, win_w, win_h, 8);

    // Window Body
    draw_filled_box(v, win_x, win_y, win_w, win_h, 7);

    // 1px Black outline
    for (int x = win_x; x < win_x + win_w; x++) {
        PUT_PIXEL(v, x, win_y, 0);
        PUT_PIXEL(v, x, win_y + win_h - 1, 0);
    }
    for (int y = win_y; y < win_y + win_h; y++) {
        PUT_PIXEL(v, win_x, y, 0);
        PUT_PIXEL(v, win_x + win_w - 1, y, 0);
    }

    // 3D Beveled highlight
    for (int x = win_x + 1; x < win_x + win_w - 1; x++) PUT_PIXEL(v, x, win_y + 1, 15);
    for (int y = win_y + 1; y < win_y + win_h - 1; y++) PUT_PIXEL(v, win_x + 1, y, 15);

    // Windows 3.1 & Mac OS 4-color modular logo badge
    int bx = win_x + 20, by = win_y + 18, bw = 36, bh = 36;
    draw_3d_frame(v, bx, by, bw, bh, 0);
    draw_filled_box(v, bx + 2, by + 2, 16, 16, 4);  // Red (top-left)
    draw_filled_box(v, bx + 18, by + 2, 16, 16, 2); // Green (top-right)
    draw_filled_box(v, bx + 2, by + 18, 16, 16, 1); // Blue (bottom-left)
    draw_filled_box(v, bx + 18, by + 18, 16, 16, 14);// Yellow (bottom-right)

    // Center pixel cat silhouette on badge
    draw_filled_box(v, bx + 12, by + 12, 12, 12, 15);
    PUT_PIXEL(v, bx + 13, by + 11, 15); // Ears
    PUT_PIXEL(v, bx + 22, by + 11, 15);
    PUT_PIXEL(v, bx + 15, by + 16, 0);  // Eyes
    PUT_PIXEL(v, bx + 20, by + 16, 0);
    PUT_PIXEL(v, bx + 17, by + 19, 0);  // Nose

    // Title and system info
    font_draw_text(v, win_x + 68, win_y + 16, "Pixel Neko Next", 0);
    font_draw_text(v, win_x + 68, win_y + 32, "Version C2.0 (Build 1750.000)", 0);
    font_draw_text(v, win_x + 68, win_y + 48, "32-Bit Graphical Operating System", 8);
    font_draw_text(v, win_x + 68, win_y + 64, "(C) 2026 Tairitsu_tty / XNZM", 8);

    // Beveled horizontal separator line
    for (int x = win_x + 14; x < win_x + win_w - 14; x++) {
        PUT_PIXEL(v, x, win_y + 84, 8);
        PUT_PIXEL(v, x, win_y + 85, 15);
    }

    // Extension parade container line at screen bottom
    int parade_y = screen_h - 30;
    font_draw_text(v, 16, parade_y - 12, "Extensions Loaded:", 8);

    copy_fullscreen_to_lfb(v);

    // Boot Steps Table
    boot_step_t steps[8] = {
        { "Initializing processor...", cpu_model_string, 0, 15, 140, NULL },
        { "Verifying memory map...", "Physical Page Bitmap OK", 1, 28, 140, NULL },
        { "Scanning PCI bus & VGA controller...", "VBE Linear Framebuffer Active", 2, 42, 140, NULL },
        { "Configuring PS/2 keyboard buffer...", "IRQ1 Remapped", 3, 56, 140, NULL },
        { "Initializing PS/2 mouse streaming...", "Auxiliary Device Enabled", 4, 70, 160, mouse_init },
        { "Probing ATA IDE storage & partitions...", "Master/Slave Drive Check", 5, 82, 180, NULL },
        { "Mounting drives & volumes (VFS)...", "Virtual File System Ready", 6, 92, 180, run_vfs_init },
        { "Loading desktop environment...", "Window Manager Ready", 7, 100, 160, wm_init }
    };

    int current_percent = 5;
    int stripe_phase = 0;
    int parade_icons_shown = 0;

    int bar_x = win_x + 20;
    int bar_y = win_y + 170;
    int bar_w = win_w - 40;
    int bar_h = 16;

    for (int step = 0; step < 8; step++) {
        if (!skip) {
            if (splash_poll_skip()) skip = 1;
        }

        // Run step execution
        if (steps[step].exec != NULL) {
            steps[step].exec();
        }

        // Add extension icon to parade at bottom
        int icon_x = 16 + parade_icons_shown * 24;
        if (icon_x + 20 < screen_w) {
            draw_extension_icon(v, icon_x, parade_y, steps[step].icon_id);
            copy_rect_to_lfb(v, icon_x, parade_y, 18, 18);
            parade_icons_shown++;
        }

        int target_p = steps[step].target_percent;
        int duration_ms = skip ? 0 : steps[step].min_ms;
        int elapsed_ms = 0;

        while (elapsed_ms <= duration_ms) {
            if (!skip && splash_poll_skip()) skip = 1;
            if (skip) {
                current_percent = target_p;
                break;
            }

            // Smooth progress interpolation
            if (current_percent < target_p) {
                current_percent++;
            }

            // Clear status text area
            draw_filled_box(v, win_x + 16, win_y + 96, win_w - 32, 64, 7);

            // Draw status text
            font_draw_text(v, win_x + 20, win_y + 102, steps[step].status, 0);
            font_draw_text(v, win_x + 20, win_y + 120, steps[step].detail, 8);

            // Draw Mac OS 9 Barber-Pole Striped Progress Bar
            draw_3d_frame(v, bar_x, bar_y, bar_w, bar_h, 1);
            draw_filled_box(v, bar_x + 1, bar_y + 1, bar_w - 2, bar_h - 2, 15);

            int fill_w = ((bar_w - 4) * current_percent) / 100;
            if (fill_w > bar_w - 4) fill_w = bar_w - 4;

            for (int py = bar_y + 2; py < bar_y + bar_h - 2; py++) {
                for (int px = bar_x + 2; px < bar_x + 2 + fill_w; px++) {
                    // 45-degree diagonal candy-cane stripes
                    int stripe = (px + py + stripe_phase) % 8;
                    unsigned char color = (stripe < 4) ? 1 : 11; // Deep Blue & Light Cyan
                    PUT_PIXEL(v, px, py, color);
                }
            }

            // Copy status + progress bar area to LFB
            copy_rect_to_lfb(v, win_x + 14, win_y + 94, win_w - 28, win_h - 100);

            stripe_phase = (stripe_phase + 1) % 8;
            elapsed_ms += 25;
            sleep_ms(25);
        }
    }

    // Final fill to 100%
    if (!skip) {
        draw_3d_frame(v, bar_x, bar_y, bar_w, bar_h, 1);
        draw_filled_box(v, bar_x + 1, bar_y + 1, bar_w - 2, bar_h - 2, 15);
        for (int py = bar_y + 2; py < bar_y + bar_h - 2; py++) {
            for (int px = bar_x + 2; px < bar_x + bar_w - 2; px++) {
                int stripe = (px + py + stripe_phase) % 8;
                unsigned char color = (stripe < 4) ? 1 : 11;
                PUT_PIXEL(v, px, py, color);
            }
        }
        font_draw_text(v, win_x + 20, win_y + 142, "Starting desktop...", 0);
        copy_rect_to_lfb(v, win_x + 14, win_y + 94, win_w - 28, win_h - 100);
        sleep_ms(150);
    }

    // ------------------------------------------------------------------------
    // Part 3: Zooming Transition to Desktop (Classic Finder Zoom Rectangles)
    // ------------------------------------------------------------------------
    if (!skip) {
        for (int frame = 0; frame < 5; frame++) {
            int zx = win_x - frame * (win_x / 5);
            int zy = win_y - frame * (win_y / 5);
            int zw = win_w + frame * ((screen_w - win_w) / 5);
            int zh = win_h + frame * ((screen_h - win_h) / 5);

            // Draw dotted outline
            for (int x = zx; x < zx + zw; x += 4) {
                PUT_PIXEL(v, x, zy, 0);
                PUT_PIXEL(v, x, zy + zh - 1, 0);
            }
            for (int y = zy; y < zy + zh; y += 4) {
                PUT_PIXEL(v, zx, y, 0);
                PUT_PIXEL(v, zx + zw - 1, y, 0);
            }
            copy_fullscreen_to_lfb(v);
            sleep_ms(20);
        }
    }
}
