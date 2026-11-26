#include "hal.h"

/* Konstanta layar & arena (160x120 piksel) */
#define SCREEN_W      160
#define SCREEN_H      120
#define HUD_H         12
#define WALL_TOP      12
#define WALL_BOT      118
#define PLAY_TOP      13
#define PLAY_BOT      117

/* Konstanta raket & bola */
#define PAD_W         3
#define PAD_H         18
#define P1_X          6
#define P2_X          151
#define BALL_SIZE     3
#define WIN_SCORE     7

/* Skala fixed-point x16 (geser 4 bit) */
#define FP_SHIFT      4
#define PAD_SPEED     32   /* 2.0 piksel/frame */
#define AI_SPEED      24   /* 1.5 piksel/frame */
#define INIT_VX       20   /* 1.25 piksel/frame */
#define MAX_VX        44   /* 2.75 piksel/frame (< PAD_W agar tidak tembus) */

/* Palet warna RGB332 */
#define COL_BLACK     0x00
#define COL_WHITE     0xFF
#define COL_RED       0xE0
#define COL_GREEN     0x1C
#define COL_BLUE      0x03
#define COL_YELLOW    0xFC
#define COL_CYAN      0x1F
#define COL_GRAY      0x92

typedef enum {
    ST_MENU,
    ST_PLAY,
    ST_PAUSE,
    ST_GAMEOVER
} state_t;

typedef struct {
    state_t state;
    int mode_ai;            /* 0 = 2 Pemain, 1 = 1 Pemain vs AI */
    int score1, score2;
    int p1_y, p2_y;         /* Posisi Y raket (fixed-point x16) */
    int old_p1_y, old_p2_y; /* Posisi Y raket sebelumnya (piksel) */
    int bx, by;             /* Posisi bola (fixed-point x16) */
    int old_bx, old_by;     /* Posisi bola sebelumnya (piksel) */
    int vx, vy;             /* Kecepatan bola (fixed-point x16) */
    int serve_dir;          /* 1 = ke kanan, -1 = ke kiri */
    int need_clear;         /* 1 = gambar ulang layar penuh saat ganti state */
    int score_dirty;        /* 1 = perbarui angka skor di HUD & 7-segment */
} game_t;

static void reset_ball(game_t *g) {
    int r = (int)(rand16() & 0x0F) - 8; /* Variasi arah awal -8..+7 */
    g->bx = ((SCREEN_W - BALL_SIZE) >> 1) << FP_SHIFT;
    g->by = (((PLAY_TOP + PLAY_BOT - BALL_SIZE) >> 1)) << FP_SHIFT;
    g->vx = (g->serve_dir > 0) ? INIT_VX : -INIT_VX;
    g->vy = (r == 0) ? 8 : r;
    g->old_bx = g->bx >> FP_SHIFT;
    g->old_by = g->by >> FP_SHIFT;
}

static void reset_match(game_t *g) {
    g->score1 = 0;
    g->score2 = 0;
    g->p1_y = (((PLAY_TOP + PLAY_BOT - PAD_H) >> 1)) << FP_SHIFT;
    g->p2_y = g->p1_y;
    g->old_p1_y = g->p1_y >> FP_SHIFT;
    g->old_p2_y = g->p2_y >> FP_SHIFT;
    g->serve_dir = (rand16() & 1) ? 1 : -1;
    reset_ball(g);
    g->score_dirty = 1;
}

static void game_init(game_t *g) {
    g->state = ST_MENU;
    g->mode_ai = 0;
    g->need_clear = 1;
    reset_match(g);
    score_show(0, 0);
    led_set(0x0001);
}

static void game_input(game_t *g) {
    if (g->state == ST_MENU) {
        if (key_pressed(KEY_1)) {
            g->mode_ai = 1;
            g->need_clear = 1;
        }
        if (key_pressed(KEY_2)) {
            g->mode_ai = 0;
            g->need_clear = 1;
        }
        if (key_pressed(KEY_SPACE)) {
            g->mode_ai = !g->mode_ai;
            g->need_clear = 1;
        }
        if (key_pressed(KEY_ENTER)) {
            reset_match(g);
            g->state = ST_PLAY;
            g->need_clear = 1;
        }
        return;
    }

    if (g->state == ST_PAUSE) {
        if (key_pressed(KEY_P) || key_pressed(KEY_ENTER)) {
            g->state = ST_PLAY;
            g->need_clear = 1;
        }
        return;
    }

    if (g->state == ST_GAMEOVER) {
        if (key_pressed(KEY_ENTER) || key_pressed(KEY_SPACE)) {
            reset_match(g);
            g->state = ST_PLAY;
            g->need_clear = 1;
        }
        return;
    }

    /* State ST_PLAY: jeda dengan tombol P */
    if (key_pressed(KEY_P)) {
        g->state = ST_PAUSE;
        g->need_clear = 1;
        return;
    }

    /* Gerak raket kiri (Pemain 1: W / S) */
    if (key_down(KEY_W)) {
        g->p1_y -= PAD_SPEED;
    }
    if (key_down(KEY_S)) {
        g->p1_y += PAD_SPEED;
    }

    /* Gerak raket kanan (Pemain 2: UP / DOWN, atau AI jika mode 1P) */
    if (!g->mode_ai) {
        if (key_down(KEY_UP)) {
            g->p2_y -= PAD_SPEED;
        }
        if (key_down(KEY_DOWN)) {
            g->p2_y += PAD_SPEED;
        }
    } else {
        int pad_mid = (g->p2_y >> FP_SHIFT) + (PAD_H >> 1);
        int ball_mid = (g->by >> FP_SHIFT) + (BALL_SIZE >> 1);
        if (g->vx > 0) {
            if (ball_mid < pad_mid - 2) {
                g->p2_y -= AI_SPEED;
            } else if (ball_mid > pad_mid + 2) {
                g->p2_y += AI_SPEED;
            }
        }
    }
}

static void game_update(game_t *g) {
    int min_pad_y = PLAY_TOP << FP_SHIFT;
    int max_pad_y = (PLAY_BOT - PAD_H + 1) << FP_SHIFT;
    int bx_px, by_px, p1_px, p2_px;

    if (g->state != ST_PLAY) {
        return;
    }

    /* Batasi posisi raket agar tidak menabrak dinding */
    if (g->p1_y < min_pad_y) g->p1_y = min_pad_y;
    if (g->p1_y > max_pad_y) g->p1_y = max_pad_y;
    if (g->p2_y < min_pad_y) g->p2_y = min_pad_y;
    if (g->p2_y > max_pad_y) g->p2_y = max_pad_y;

    /* Gerakkan bola */
    g->bx += g->vx;
    g->by += g->vy;

    bx_px = g->bx >> FP_SHIFT;
    by_px = g->by >> FP_SHIFT;
    p1_px = g->p1_y >> FP_SHIFT;
    p2_px = g->p2_y >> FP_SHIFT;

    /* Pantulan dinding atas & bawah */
    if (by_px <= PLAY_TOP) {
        g->by = PLAY_TOP << FP_SHIFT;
        g->vy = -g->vy;
        by_px = PLAY_TOP;
    } else if (by_px + BALL_SIZE - 1 >= PLAY_BOT) {
        g->by = (PLAY_BOT - BALL_SIZE + 1) << FP_SHIFT;
        g->vy = -g->vy;
        by_px = PLAY_BOT - BALL_SIZE + 1;
    }

    /* Tumbukan dengan raket kiri (P1) */
    if (g->vx < 0 &&
        bx_px <= P1_X + PAD_W &&
        bx_px + BALL_SIZE >= P1_X &&
        by_px + BALL_SIZE > p1_px &&
        by_px < p1_px + PAD_H) {
        int diff = (by_px + (BALL_SIZE >> 1)) - (p1_px + (PAD_H >> 1));
        int next_vx = -g->vx + 2;
        if (next_vx > MAX_VX) next_vx = MAX_VX;
        g->vx = next_vx;
        g->vy = diff << 1;
        g->bx = (P1_X + PAD_W) << FP_SHIFT;
    }

    /* Tumbukan dengan raket kanan (P2 / AI) */
    if (g->vx > 0 &&
        bx_px + BALL_SIZE >= P2_X &&
        bx_px <= P2_X + PAD_W &&
        by_px + BALL_SIZE > p2_px &&
        by_px < p2_px + PAD_H) {
        int diff = (by_px + (BALL_SIZE >> 1)) - (p2_px + (PAD_H >> 1));
        int next_vx = -g->vx - 2;
        if (next_vx < -MAX_VX) next_vx = -MAX_VX;
        g->vx = next_vx;
        g->vy = diff << 1;
        g->bx = (P2_X - BALL_SIZE) << FP_SHIFT;
    }

    /* Cek skor jika bola melewati batas kiri/kanan */
    bx_px = g->bx >> FP_SHIFT;
    if (bx_px <= 1) {
        g->score2++;
        g->serve_dir = -1;
        g->score_dirty = 1;
        if (g->score2 >= WIN_SCORE) {
            g->state = ST_GAMEOVER;
            g->need_clear = 1;
        } else {
            /* Hapus bola lama sebelum reset ke tengah */
            fb_rect(g->old_bx, g->old_by, BALL_SIZE, BALL_SIZE, COL_BLACK);
            reset_ball(g);
        }
    } else if (bx_px + BALL_SIZE >= SCREEN_W - 1) {
        g->score1++;
        g->serve_dir = 1;
        g->score_dirty = 1;
        if (g->score1 >= WIN_SCORE) {
            g->state = ST_GAMEOVER;
            g->need_clear = 1;
        } else {
            fb_rect(g->old_bx, g->old_by, BALL_SIZE, BALL_SIZE, COL_BLACK);
            reset_ball(g);
        }
    }
}

static void draw_hud_and_arena(const game_t *g) {
    int y;
    /* Garis batas atas dan bawah */
    fb_rect(0, WALL_TOP, SCREEN_W, 1, COL_WHITE);
    fb_rect(0, WALL_BOT, SCREEN_W, 1, COL_WHITE);

    /* Garis putus-putus di tengah arena */
    for (y = PLAY_TOP + 2; y < PLAY_BOT - 2; y += 6) {
        fb_rect((SCREEN_W >> 1), y, 1, 3, COL_GRAY);
    }

    /* Teks label mode di tengah atas */
    if (g->mode_ai) {
        fb_text(68, 3, "1P-AI", COL_YELLOW);
    } else {
        fb_text(74, 3, "2P", COL_CYAN);
    }
}

static void game_draw(const game_t *g) {
    game_t *gm = (game_t *)g;

    if (g->state == ST_MENU) {
        if (g->need_clear) {
            int w_title = fb_text_width("PONG RV32", 2);
            fb_clear(COL_BLACK);
            fb_frame(2, 2, SCREEN_W - 4, SCREEN_H - 4, COL_CYAN);
            fb_text_big((SCREEN_W - w_title) >> 1, 14, "PONG RV32", COL_YELLOW, 2);
            fb_text(44, 32, "KELOMPOK 02 - D", COL_WHITE);

            if (g->mode_ai) {
                fb_text(30, 50, "> 1P VS CPU (TEKAN 1)", COL_GREEN);
                fb_text(30, 60, "  2 PEMAIN  (TEKAN 2)", COL_GRAY);
            } else {
                fb_text(30, 50, "  1P VS CPU (TEKAN 1)", COL_GRAY);
                fb_text(30, 60, "> 2 PEMAIN  (TEKAN 2)", COL_GREEN);
            }

            fb_text(22, 78, "P1: W/S   P2: ATAS/BAWAH", COL_CYAN);
            fb_text(34, 88, "TARGET SKOR: 7 POIN", COL_WHITE);
            fb_text(26, 102, "TEKAN ENTER UNTUK MULAI", COL_YELLOW);
            gm->need_clear = 0;
        }
        return;
    }

    if (g->state == ST_PAUSE) {
        if (g->need_clear) {
            int w_pause = fb_text_width("JEDA", 2);
            fb_rect(34, 42, 92, 36, COL_BLUE);
            fb_frame(34, 42, 92, 36, COL_WHITE);
            fb_text_big((SCREEN_W - w_pause) >> 1, 48, "JEDA", COL_YELLOW, 2);
            fb_text(40, 66, "TEKAN P LANJUT", COL_WHITE);
            gm->need_clear = 0;
        }
        return;
    }

    if (g->state == ST_GAMEOVER) {
        if (g->need_clear) {
            int w_over = fb_text_width("SELESAI", 2);
            fb_clear(COL_BLACK);
            fb_frame(4, 4, SCREEN_W - 8, SCREEN_H - 8, COL_YELLOW);
            fb_text_big((SCREEN_W - w_over) >> 1, 18, "SELESAI", COL_RED, 2);

            if (g->score1 >= WIN_SCORE) {
                fb_text(42, 44, "PEMAIN 1 MENANG!", COL_GREEN);
            } else if (g->mode_ai) {
                fb_text(46, 44, "KOMPUTER MENANG!", COL_CYAN);
            } else {
                fb_text(42, 44, "PEMAIN 2 MENANG!", COL_CYAN);
            }

            fb_text(48, 62, "SKOR:", COL_WHITE);
            fb_number(74, 62, g->score1, COL_GREEN);
            fb_text(86, 62, "-", COL_WHITE);
            fb_number(94, 62, g->score2, COL_CYAN);

            fb_text(24, 92, "TEKAN ENTER MAIN LAGI", COL_YELLOW);
            score_show(g->score1, g->score2);
            gm->need_clear = 0;
        }
        return;
    }

    /* State ST_PLAY: gambar ulang penuh hanya saat baru mulai/lanjut dari jeda */
    if (g->need_clear) {
        fb_clear(COL_BLACK);
        draw_hud_and_arena(g);
        gm->need_clear = 0;
        gm->score_dirty = 1;
    }

    /* Perbarui papan skor & 7-segment hanya saat skor berubah */
    if (g->score_dirty) {
        int led_p1 = (1 << g->score1) - 1;
        int led_p2 = ((1 << g->score2) - 1) << (16 - g->score2);
        fb_rect(6, 2, 45, 8, COL_BLACK);
        fb_rect(110, 2, 45, 8, COL_BLACK);
        fb_text(8, 3, "P1:", COL_GREEN);
        fb_number(24, 3, g->score1, COL_WHITE);
        fb_text(114, 3, "P2:", COL_CYAN);
        fb_number(130, 3, g->score2, COL_WHITE);
        score_show(g->score1, g->score2);
        led_set((uint16_t)(led_p1 | led_p2));
        gm->score_dirty = 0;
    }

    /* Hapus posisi lama raket & bola (anti-kedip) */
    int cur_p1_y = g->p1_y >> FP_SHIFT;
    int cur_p2_y = g->p2_y >> FP_SHIFT;
    int cur_bx   = g->bx >> FP_SHIFT;
    int cur_by   = g->by >> FP_SHIFT;

    if (cur_p1_y != g->old_p1_y) {
        fb_rect(P1_X, g->old_p1_y, PAD_W, PAD_H, COL_BLACK);
    }
    if (cur_p2_y != g->old_p2_y) {
        fb_rect(P2_X, g->old_p2_y, PAD_W, PAD_H, COL_BLACK);
    }
    fb_rect(g->old_bx, g->old_by, BALL_SIZE, BALL_SIZE, COL_BLACK);

    /* Pulihkan garis tengah jika terhapus lintasan bola */
    if (g->old_bx <= (SCREEN_W >> 1) && g->old_bx + BALL_SIZE >= (SCREEN_W >> 1)) {
        int y;
        for (y = PLAY_TOP + 2; y < PLAY_BOT - 2; y += 6) {
            fb_rect((SCREEN_W >> 1), y, 1, 3, COL_GRAY);
        }
    }

    /* Gambar posisi baru raket & bola */
    fb_rect(P1_X, cur_p1_y, PAD_W, PAD_H, COL_GREEN);
    fb_rect(P2_X, cur_p2_y, PAD_W, PAD_H, COL_CYAN);
    fb_rect(cur_bx, cur_by, BALL_SIZE, BALL_SIZE, COL_WHITE);

    gm->old_p1_y = cur_p1_y;
    gm->old_p2_y = cur_p2_y;
    gm->old_bx   = cur_bx;
    gm->old_by   = cur_by;
}

int main(void) {
    game_t g;
    hal_init();
    game_init(&g);
    for (;;) {
        wait_vsync();
        game_input(&g);
        game_update(&g);
        game_draw(&g);
    }
}