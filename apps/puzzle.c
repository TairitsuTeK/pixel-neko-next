#include "puzzle.h"
#include "font.h"
#include "wm.h"
#include "speaker.h"

extern volatile int global_tick;
extern int redraw;

static int board[4][4];
static int empty_r = 3, empty_c = 3;
static int moves = 0;
static int is_won = 0;

static unsigned int rng_state = 123456789;

static unsigned int puzzle_rand(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static void check_win(void) {
    int expected = 1;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (r == 3 && c == 3) {
                if (board[r][c] != 0) return;
            } else {
                if (board[r][c] != expected) return;
                expected++;
            }
        }
    }
    is_won = 1;
    speaker_beep();
}

void puzzle_init(void) {
    int val = 1;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            board[r][c] = val++;
        }
    }
    board[3][3] = 0;
    empty_r = 3;
    empty_c = 3;
    moves = 0;
    is_won = 0;

    rng_state ^= (unsigned int)global_tick + 0xDEADBEEF;

    // Perform valid random legal moves to ensure solvability
    const int dr[4] = { -1, 1, 0, 0 };
    const int dc[4] = { 0, 0, -1, 1 };
    int last_move = -1;

    for (int i = 0; i < 150; i++) {
        int valid_moves[4];
        int num_valid = 0;
        for (int m = 0; m < 4; m++) {
            // Avoid immediately undoing the last move
            if (last_move != -1 && (m ^ 1) == last_move) continue;
            int nr = empty_r + dr[m];
            int nc = empty_c + dc[m];
            if (nr >= 0 && nr < 4 && nc >= 0 && nc < 4) {
                valid_moves[num_valid++] = m;
            }
        }
        if (num_valid == 0) continue;
        int chosen = valid_moves[puzzle_rand() % num_valid];
        int nr = empty_r + dr[chosen];
        int nc = empty_c + dc[chosen];
        board[empty_r][empty_c] = board[nr][nc];
        board[nr][nc] = 0;
        empty_r = nr;
        empty_c = nc;
        last_move = chosen;
    }
}

void puzzle_draw(unsigned char* v, int wx, int wy, int ww, int wh) {
    // Fill window interior
    for (int y = wy + 20; y < wy + wh - 1; y++) {
        for (int x = wx + 1; x < wx + ww - 1; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }

    int tile_size = 32;
    int grid_w = 4 * tile_size;
    int start_x = wx + (ww - grid_w) / 2;
    int start_y = wy + 24;

    // Header: Moves or Solved!
    char stat_str[32] = "Moves: ";
    int m = moves;
    char num_buf[12];
    int idx = 0;
    if (m == 0) num_buf[idx++] = '0';
    else {
        char tmp[12]; int ti = 0;
        while (m > 0) { tmp[ti++] = '0' + (m % 10); m /= 10; }
        while (ti > 0) num_buf[idx++] = tmp[--ti];
    }
    num_buf[idx] = '\0';
    for (int i = 0; num_buf[i]; i++) {
        stat_str[7 + i] = num_buf[i];
        stat_str[8 + i] = '\0';
    }

    if (is_won) {
        font_draw_text(v, start_x + 8, start_y - 12, "SOLVED! CONGRATULATIONS!", 2);
    } else {
        font_draw_text(v, start_x + 4, start_y - 12, stat_str, 0);
    }

    // Grid border
    for (int x = start_x - 2; x <= start_x + grid_w + 1; x++) {
        PUT_PIXEL(v, x, start_y - 2, 0);
        PUT_PIXEL(v, x, start_y + grid_w + 1, 15);
    }
    for (int y = start_y - 2; y <= start_y + grid_w + 1; y++) {
        PUT_PIXEL(v, start_x - 2, y, 0);
        PUT_PIXEL(v, start_x + grid_w + 1, y, 15);
    }

    // Draw tiles
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int val = board[r][c];
            int tx = start_x + c * tile_size;
            int ty = start_y + r * tile_size;

            if (val == 0) {
                // Empty sunken slot
                for (int y = ty; y < ty + tile_size; y++) {
                    for (int x = tx; x < tx + tile_size; x++) {
                        PUT_PIXEL(v, x, y, 8);
                    }
                }
            } else {
                // 3D Beveled Tile
                for (int y = ty; y < ty + tile_size; y++) {
                    for (int x = tx; x < tx + tile_size; x++) {
                        PUT_PIXEL(v, x, y, 7);
                    }
                }
                // Highlight top & left
                for (int x = tx; x < tx + tile_size; x++) {
                    PUT_PIXEL(v, x, ty, 15);
                }
                for (int y = ty; y < ty + tile_size; y++) {
                    PUT_PIXEL(v, tx, y, 15);
                }
                // Shadow bottom & right
                for (int x = tx; x < tx + tile_size; x++) {
                    PUT_PIXEL(v, x, ty + tile_size - 1, 0);
                }
                for (int y = ty; y < ty + tile_size; y++) {
                    PUT_PIXEL(v, tx + tile_size - 1, y, 0);
                }

                // Number string
                char s[3] = {0};
                if (val < 10) {
                    s[0] = '0' + val;
                } else {
                    s[0] = '1';
                    s[1] = '0' + (val - 10);
                }
                int text_x = tx + (val < 10 ? 12 : 8);
                int text_y = ty + (tile_size - 12) / 2;
                font_draw_text(v, text_x, text_y, s, 0);
            }
        }
    }

    // [ New Game ] Button at bottom
    int btn_w = 80, btn_h = 20;
    int btn_x = wx + (ww - btn_w) / 2;
    int btn_y = start_y + grid_w + 8;

    for (int y = btn_y; y < btn_y + btn_h; y++) {
        for (int x = btn_x; x < btn_x + btn_w; x++) {
            PUT_PIXEL(v, x, y, 7);
        }
    }
    for (int x = btn_x; x < btn_x + btn_w; x++) {
        PUT_PIXEL(v, x, btn_y, 15);
        PUT_PIXEL(v, x, btn_y + btn_h - 1, 0);
    }
    for (int y = btn_y; y < btn_y + btn_h; y++) {
        PUT_PIXEL(v, btn_x, y, 15);
        PUT_PIXEL(v, btn_x + btn_w - 1, y, 0);
    }
    font_draw_text(v, btn_x + 12, btn_y + 4, "New Game", 0);
}

void puzzle_click(int mx, int my, int wx, int wy, int ww, int wh) {
    int tile_size = 32;
    int grid_w = 4 * tile_size;
    int start_x = wx + (ww - grid_w) / 2;
    int start_y = wy + 24;

    // Check [ New Game ] Button
    int btn_w = 80, btn_h = 20;
    int btn_x = wx + (ww - btn_w) / 2;
    int btn_y = start_y + grid_w + 8;
    if (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h) {
        puzzle_init();
        redraw = 1;
        return;
    }

    // Check tiles
    if (mx >= start_x && mx < start_x + grid_w && my >= start_y && my < start_y + grid_w) {
        int c = (mx - start_x) / tile_size;
        int r = (my - start_y) / tile_size;
        if (r >= 0 && r < 4 && c >= 0 && c < 4) {
            int dr = r - empty_r;
            int dc = c - empty_c;
            if (dr < 0) dr = -dr;
            if (dc < 0) dc = -dc;
            if ((dr == 1 && dc == 0) || (dr == 0 && dc == 1)) {
                // Legal move!
                board[empty_r][empty_c] = board[r][c];
                board[r][c] = 0;
                empty_r = r;
                empty_c = c;
                moves++;
                check_win();
                redraw = 1;
            }
        }
    }
}
