/* Gyre & Gimble, ported from the Commander X16 version (x16/src/main.asm).
 *
 * Rotate the rings until a tunnel runs from the entry gap at the top of the
 * outer ring to the burrow in the middle. The routines here follow main.asm
 * one for one, with the same names and the same 8-bit arithmetic, and draw
 * through the VERA model (vera.c) the way the X16 version draws through
 * VERA. All the data (levels, pieces, palettes, sprites, sounds, music)
 * comes from the X16 repo's tools, via gg_data.h. */
#include "game.h"

#include <string.h>

#include "audio.h"
#include "gg_data.h"
#include "music.h"
#include "vera.h"

/* Tove sprite */
#define TOVE_SPRITE 1               /* sprite 0 is the KERNAL mouse pointer */
#define TOVE_PAL ROW_TOVE
#define TOVE_Z (2 << 2)             /* Z-depth 2: over the board, under the HUD text */
#define WALK_STEPS 8

/* Sound effects */
#define AUDIO_VOLUME 12
#define AUDIO_RESET 0x80
#define SOUND_BANK_SIZE 8192

/* Garden critters: borogoves and mome raths */
#define CRITTER_SPRITE 2
#define NCRITTERS 4
#define CRITTER_PAL ROW_CRITTER
#define BOROGOVE 0
#define RATH 4
#define POSE_WALK 0
#define POSE_IDLE 2
#define POSE_CHEER 3

/* Cell bits (match tools/level.py) */
#define IN 0x01
#define OUT 0x02
#define CW 0x04
#define TUN 0x80

#define RINGS 4
#define CELLS 12
#define NCELLS (RINGS * CELLS)
#define ENTRY_K ((RINGS - 1) * CELLS)

#define CCW 0x08
#define KEY_CLASS 0x70

#define PALETTE_ROW 32

/* CRT title-safe text area */
#define SAFE_L 4
#define SAFE_R 35
#define SAFE_T 3
#define SAFE_B 26

#define STAR_SCREEN 0x70
#define CLR_LIT COLOR_YELLOW
#define CLR_DIM COLOR_GRAY
#define CLR_TEXT COLOR_WHITE

#define JOY_DELAY 18
#define JOY_RATE 8
#define JOY_DPAD 0x0f

/* The ending */
#define ENDING_ROW 10
#define ENDING_COL 10
#define ENDING_W 20
#define ENDING_H 10
#define CLR_PANEL COLOR_DARK_GRAY

#define CELLS_BANK_SIZE 8192

_Static_assert(ROW_TUNNEL == ROW_STONE + RINGS, "class rows must be contiguous");
_Static_assert(ROW_LIT == ROW_TUNNEL + 1, "class rows must be contiguous");
_Static_assert(ENTRY_SLOT == BURROW_SLOT + BURROW_SLOTS, "fixed row layout");
_Static_assert(TH_HOME - TH_BURROW == BURROW_SLOTS * 2, "fixed row layout");
_Static_assert(CRITTER_TARGETS == 16, "band_target assumes 16 waypoints");

/* Each critter: its kind, band (0 left, 1 right), frames per step - 1
 * (a mask of anim) and starting waypoint */
static const uint8_t cr_kind[NCRITTERS] = {BOROGOVE, RATH, RATH, BOROGOVE};
static const uint8_t cr_band[NCRITTERS] = {0, 0, 1, 1};
static const uint8_t cr_pace[NCRITTERS] = {1, 3, 3, 1};
static const uint8_t cr_start[NCRITTERS] = {3, 9, 5, 12};

/* The HUD's words. The touch versions name the gestures and on-screen
 * buttons in place of the keys. */
typedef struct {
    const char *help1, *help2, *help3, *help4, *help5;
    const char *next1, *next2, *end1, *end2, *win6;
} Words;

static const Words keyboard_words = {
    "W/S ring", "A/D turn", "U undo", "R reset", "Q quit",
    "  Return", "for next", "  Return", " to win!", "Return: again",
};
static const Words touch_words = {
    "Tap ring", "to pick,", "swipe to", "turn it.", "",
    "Tap Next", "to go on", "Tap Next", " to win!", "Tap Next: again",
};
static const Words *words = &keyboard_words;

static const char s_title1[] = "Gyre &";
static const char s_title2[] = "Gimble";
static const char s_moves[] = "Moves";
static const char s_par[] = "Par   ";
static const char s_home1[] = "The tove";
static const char s_home2[] = "is home!";
static const char s_level[] = "Level ";
static const char s_best[] = "Best";
static const char s_blank[] = "        ";
static const char s_win1[] = "O frabjous day!";
static const char s_win2[] = "Callooh! Callay!";
static const char s_win3[] = "You solved all ";
static const char s_win4[] = "levels!";
static const char s_win5[] = "Stars ";

/* State (main.asm's BSS) */
static uint16_t seed;
static uint8_t finished;
static uint8_t music_off;
static uint8_t critter_z;
static uint8_t cr_x[NCRITTERS], cr_y[NCRITTERS], cr_tx[NCRITTERS], cr_ty[NCRITTERS];
static uint8_t cr_walk[NCRITTERS], cr_timer[NCRITTERS], cr_flip[NCRITTERS];
static uint8_t parent[NCELLS], queue[NCELLS], q_head, q_tail, goal;
static uint8_t path[NCELLS], path_n, prev_k;
static uint8_t wp_x[2 * NCELLS + 4], wp_y[2 * NCELLS + 4], wp_n, wp_i;
static uint8_t walking, arrived;
static uint16_t posx, posy;         /* 8.8 fixed point */
static uint16_t stepx, stepy;
static uint8_t seg_count, anim;
static uint8_t tove_x, tove_y, tove_frame, tove_flip, tove_z;
static uint8_t joy0_prev, joy1_prev, joy_timer;
static uint8_t level;
static uint8_t lvl[LEVEL_SIZE];
static uint8_t ring_stone[RINGS];
static uint8_t drawn[NCELLS];
static uint8_t off[RINGS];
static uint8_t scr[NCELLS];
static uint8_t vis[NCELLS];
static uint8_t sel;
static uint8_t moves;
static uint8_t won;
static uint8_t stars;
static uint8_t best[NLEVELS];
static uint8_t hist_n;
static uint8_t hist[256];
static const uint8_t *theme;        /* the level's theme palette data */
static bool progress_changed;

static void refresh(void);
static void reset_level(void);
static void draw_status(void);
static void update_palette(void);

static uint8_t ring_of(int k) { return (uint8_t)(k / CELLS); }
static uint8_t pos_of(int k) { return (uint8_t)(k % CELLS); }
static uint8_t ccw_of(int k) { return (uint8_t)(k / CELLS * CELLS + (k + CELLS - 1) % CELLS); }
static uint8_t ring_class(int r) { return (uint8_t)((ROW_STONE + r) << 4); }

/* --- text: the KERNAL's PLOT and CHROUT, for what the game prints --------- */

static int cur_row, cur_col;
static uint8_t cur_fg = CLR_TEXT, cur_bg = COLOR_BLACK;

static void plot(int row, int col)
{
    cur_row = row;
    cur_col = col;
}

static void put_code(uint8_t code)
{
    if (cur_row >= 0 && cur_row < TEXT_ROWS && cur_col >= 0 && cur_col < TEXT_COLS) {
        vera.text_code[cur_row][cur_col] = code;
        vera.text_attr[cur_row][cur_col] = (uint8_t)(cur_bg << 4 | cur_fg);
    }
    if (++cur_col == TEXT_COLS) {
        cur_col = 0;
        cur_row++;
    }
}

/* ASCII to a screen code of the Upper/Lower charset */
static uint8_t screen_code(char c)
{
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z')
        return (uint8_t)(c - 'A' + 0x41);
    if (c == '@')
        return 0;
    if (c >= 0x20 && c < 0x40)
        return (uint8_t)c;
    return 0x20;
}

static void print_str(const char *s)
{
    while (*s)
        put_code(screen_code(*s++));
}

static void print_at(int row, int col, const char *s)
{
    plot(row, col);
    print_str(s);
}

static void clear_screen(void)
{
    memset(vera.text_code, 0x20, sizeof vera.text_code);
    memset(vera.text_attr, cur_bg << 4 | cur_fg, sizeof vera.text_attr);
    plot(0, 0);
}

/* Print n (0-999) in decimal at the cursor, without leading zeros */
static void print_num(int n)
{
    int h = n / 100, t = n / 10 % 10;
    if (h)
        put_code((uint8_t)('0' + h));
    if (h || t)
        put_code((uint8_t)('0' + t));
    put_code((uint8_t)('0' + n % 10));
}

/* Print n as three decimal digits at the cursor */
static void print_dec3(uint8_t n)
{
    put_code((uint8_t)('0' + n / 100));
    put_code((uint8_t)('0' + n / 10 % 10));
    put_code((uint8_t)('0' + n % 10));
}

/* --- sound ---------------------------------------------------------------- */

/* Play sound n, cutting off any sound still playing. Each sound fits VERA's
 * 4 KB FIFO, so it goes in whole and plays on by itself. */
static void play_sound(int n)
{
    const uint8_t *p = gg_bin_sounds + D_snd_bank[n] * SOUND_BANK_SIZE +
                       ((D_snd_hi[n] << 8 | D_snd_lo[n]) - 0xa000);
    int len = D_snd_len_hi[n] << 8 | D_snd_len_lo[n];
    audio_lock();
    pcm_write_ctrl(AUDIO_RESET | AUDIO_VOLUME);     /* 8-bit mono */
    for (int i = 0; i < len; i++)
        pcm_write_fifo(p[i]);
    pcm_write_rate(SOUND_RATE);
    audio_unlock();
}

/* A tove footstep, left or right, unless another sound is still playing.
 * (main.asm tests anim & SND_STEP1, which is always 0 here; alternating on
 * anim & 8 gives the left-right patter its comment describes.) */
static void footstep(void)
{
    audio_lock();
    bool empty = pcm_fifo_empty();
    audio_unlock();
    if (empty)
        play_sound(anim & 8 ? SND_STEP2 : SND_STEP1);
}

/* Ask for the level theme's tune: none if the player turned music off, or
 * once the tove is home (the fanfare has played) */
static void want_music(void)
{
    uint8_t tune = NO_TUNE;
    if (!music_off && !arrived)
        tune = D_theme_tune[lvl[LV_THEME]];
    audio_lock();
    music_want(tune);
    audio_unlock();
}

static void want_fanfare(void)
{
    audio_lock();
    music_want(music_off ? NO_TUNE : TUNE_FANFARE);
    audio_unlock();
}

/* --- the tove --------------------------------------------------------------- */

static void init_tove(void)
{
    vera_palette_write((TOVE_PAL * 16 + 1) * 2, D_tove_pal, (int)(D_tove_frames - D_tove_pal));
}

static void set_tove(void)
{
    Sprite *sp = &vera.sprites[TOVE_SPRITE];
    sp->image = D_tove_frames + tove_frame * 128;
    sp->x = tove_x;
    sp->y = tove_y;
    sp->z = tove_z >> 2;
    sp->hflip = tove_flip & 1;
    sp->pal = TOVE_PAL;
}

/* Stand the tove at the entry gap */
static void tove_idle(void)
{
    tove_x = ENTRY_X - 8;
    tove_y = ENTRY_Y - 8;
    tove_flip = 0;
    tove_z = TOVE_Z;
    set_tove();
}

static void add_wp(uint8_t x, uint8_t y)
{
    wp_x[wp_n] = x;
    wp_y[wp_n] = y;
    wp_n++;
}

static void add_center(int k)
{
    add_wp(D_wp_cx[k], D_wp_cy[k]);
}

/* Append the gate between cells prev_k and k */
static void add_gate(int k)
{
    uint8_t d = (uint8_t)(k - prev_k);
    if (d == CELLS)                     /* k is the outer cell: its inner gate */
        add_wp(D_wp_rx[k], D_wp_ry[k]);
    else if (d == (uint8_t)-CELLS)
        add_wp(D_wp_rx[prev_k], D_wp_ry[prev_k]);
    else if (d == 1 || d == (uint8_t)-(CELLS - 1))  /* k is clockwise of prev_k */
        add_wp(D_wp_ax[prev_k], D_wp_ay[prev_k]);
    else                                /* k is counter-clockwise: its arc gate */
        add_wp(D_wp_ax[k], D_wp_ay[k]);
}

/* A signed pixel distance -> an 8.8 step per frame (distance * 256 / 8) */
static uint16_t make_step(uint8_t d)
{
    return (uint16_t)((int8_t)d * 32);
}

static void arrive(void);

/* Start walking from waypoint wp_i to wp_i + 1 */
static void start_segment(void)
{
    int x = wp_i + 1;
    if (x >= wp_n) {
        arrive();
        return;
    }
    posx = (uint16_t)(wp_x[x - 1] << 8);
    posy = (uint16_t)(wp_y[x - 1] << 8);
    uint8_t dx = (uint8_t)(wp_x[x] - wp_x[x - 1]);
    if (dx)                             /* straight up/down: keep facing */
        tove_flip = dx >> 7;            /* moving left = flip */
    stepx = make_step(dx);
    stepy = make_step((uint8_t)(wp_y[x] - wp_y[x - 1]));
    seg_count = WALK_STEPS;
}

static void next_segment(void)
{
    wp_i++;
    start_segment();
}

/* Build the waypoint list for the path ending at goal and start walking */
static void start_walk(void)
{
    int x = goal, y = 0;
    for (;;) {                          /* path cells, goal back to the entry */
        path[y++] = (uint8_t)x;
        if (x == ENTRY_K)
            break;
        x = parent[x];
    }
    path_n = (uint8_t)y;
    wp_n = 0;
    add_wp(ENTRY_X, ENTRY_Y);
    y = path_n - 1;                     /* the entry cell */
    x = path[y];
    add_center(x);
    while (--y >= 0) {
        prev_k = (uint8_t)x;
        x = path[y];
        add_gate(x);
        add_center(x);
    }
    add_wp(D_wp_rx[goal], D_wp_ry[goal]);   /* through the burrow gate, into the burrow */
    add_wp(BURROW_X, BURROW_Y);
    walking = 1;
    arrived = 0;
    wp_i = 0;
    start_segment();
}

/* Once per frame: move and animate the tove */
static void tick_tove(void)
{
    anim++;
    if (walking) {
        posx = (uint16_t)(posx + stepx);
        posy = (uint16_t)(posy + stepy);
        if (--seg_count == 0) {
            next_segment();
            if (!walking)
                return;                 /* arrived */
        }
        if ((anim & 7) == 0)
            footstep();                 /* a step every other frame */
        tove_frame = (anim >> 2) & 3;   /* walk cycle: frames 0-3, 4 jiffies each */
        tove_x = (uint8_t)((posx >> 8) - 8);
        tove_y = (uint8_t)((posy >> 8) - 8);
        set_tove();
    } else {
        if (arrived)
            return;
        tove_frame = (anim & 0x10) >> 3;    /* idle: tail turns every 16 jiffies */
        set_tove();
    }
}

/* A = stars (1-3) earned by solving the level in `moves` */
static uint8_t rate(void)
{
    int par = lvl[LV_PAR];
    if (par >= moves)
        return 3;
    int limit = par + (par >> 1);
    if (limit > 255 || limit >= moves)
        return 2;
    return 1;
}

/* The tove is home: hide it, light the burrow, show the message */
static void arrive(void)
{
    walking = 0;
    arrived = 1;
    stars = rate();
    if (stars >= best[level]) {
        if (stars != best[level])
            progress_changed = true;
        best[level] = stars;
    }
    tove_z = 0;
    set_tove();
    want_fanfare();
    update_palette();
    draw_status();
}

/* --- the critters ------------------------------------------------------------ */

/* A pseudo-random byte (16-bit Galois LFSR) */
static uint8_t random_byte(void)
{
    uint8_t a = seed & 0xff, hi = seed >> 8;
    for (int y = 8; y; y--) {
        uint8_t c = a >> 7;
        a <<= 1;
        uint8_t c2 = hi >> 7;
        hi = (uint8_t)(hi << 1 | c);
        if (c2)
            a ^= 0x39;
    }
    seed = (uint16_t)(hi << 8 | a);
    return a;
}

static int band_target(int x, int a)
{
    return cr_band[x] << 4 | a;
}

/* Put critter x at waypoint a of its band */
static void waypoint(int x, int a)
{
    int y = band_target(x, a);
    cr_x[x] = D_band_tx[y];
    cr_y[x] = D_band_ty[y];
}

/* Head critter x for waypoint a of its band */
static void target(int x, int a)
{
    int y = band_target(x, a);
    cr_tx[x] = D_band_tx[y];
    cr_ty[x] = D_band_ty[y];
}

/* Show critter x's sprite, frame f */
static void set_critter(int x, int f)
{
    Sprite *sp = &vera.sprites[CRITTER_SPRITE + x];
    int b = cr_band[x];
    sp->image = D_critters_frames + f * 128;
    sp->x = cr_x[x] + (D_band_x[b * 2] | D_band_x[b * 2 + 1] << 8);
    sp->y = cr_y[x];
    sp->z = critter_z >> 2;
    sp->hflip = cr_flip[x] & 1;
    sp->pal = CRITTER_PAL;
}

static void init_critters(uint32_t rnd)
{
    vera_palette_write((CRITTER_PAL * 16 + 1) * 2, D_critters_pal,
                       (int)(D_critters_frames - D_critters_pal));
    seed = (uint16_t)((rnd & 0xff) | 1);   /* seed the wandering (never zero) */
    critter_z = TOVE_Z;
    for (int x = NCRITTERS - 1; x >= 0; x--) {
        waypoint(x, cr_start[x]);       /* start paused at a waypoint */
        cr_tx[x] = cr_x[x];
        cr_ty[x] = cr_y[x];
        cr_timer[x] = (uint8_t)(cr_start[x] << 3);     /* staggered */
        cr_walk[x] = 0;
    }
}

/* Once per frame: move, animate and draw every critter */
static void tick_critters(void)
{
    for (int x = NCRITTERS - 1; x >= 0; x--) {
        int pose;
        if (arrived) {                  /* cheer, facing the board */
            cr_flip[x] = cr_band[x];
            pose = (anim & 0x10) ? POSE_CHEER : 0;
        } else if (cr_walk[x]) {
            if ((anim & cr_pace[x]) == 0) {     /* a step toward the waypoint */
                if (cr_x[x] != cr_tx[x]) {
                    if (cr_x[x] < cr_tx[x]) {
                        cr_x[x]++;
                        cr_flip[x] = 0;
                    } else {
                        cr_x[x]--;
                        cr_flip[x] = 1;
                    }
                }
                if (cr_y[x] != cr_ty[x]) {
                    if (cr_y[x] < cr_ty[x])
                        cr_y[x]++;
                    else
                        cr_y[x]--;
                } else if (cr_x[x] == cr_tx[x]) {
                    cr_walk[x] = 0;     /* there: pause for 40-167 frames */
                    cr_timer[x] = (uint8_t)((random_byte() & 0x7f) + 40);
                }
            }
            pose = (anim >> 3) & 1;     /* walk cycle: 8 jiffies a step */
        } else if (--cr_timer[x] == 0) {
            target(x, random_byte() & (CRITTER_TARGETS - 1));  /* off to another waypoint */
            cr_walk[x] = 1;
            pose = POSE_WALK;
        } else {
            pose = (cr_timer[x] & 0x20) ? POSE_IDLE : 0;    /* peck or sniff now and then */
        }
        set_critter(x, pose + cr_kind[x]);
    }
}

/* Load the level theme's garden colors into the palette rows from ROW_GARDEN */
static void load_garden(void)
{
    int t = lvl[LV_THEME];
    vera_palette_write(ROW_GARDEN * PALETTE_ROW,
                       gg_inc_themes + (D_garden_lo[t] | D_garden_hi[t] << 8), GARDEN_SIZE);
}

/* --- the ending ---------------------------------------------------------------- */

static int total_stars(void)
{
    int n = 0;
    for (int i = 0; i < NLEVELS; i++)
        n += best[i];
    return n;
}

static void show_ending(void)
{
    finished = 1;
    if (!music_off) {                   /* the fanfare once more */
        audio_lock();
        music_want(TUNE_FANFARE);
        music_again();
        audio_unlock();
    }
    cur_bg = CLR_PANEL;                 /* draw on a dark box: background = panel */
    cur_fg = CLR_TEXT;
    for (int row = ENDING_H - 1; row >= 0; row--) {
        plot(ENDING_ROW + row, ENDING_COL);
        for (int i = 0; i < ENDING_W; i++)
            put_code(0x20);
    }
    print_at(SAFE_B - 2, SAFE_R - 7, s_blank);  /* no more "Return to win!" */
    print_at(SAFE_B - 1, SAFE_R - 7, s_blank);
    print_at(ENDING_ROW + 1, ENDING_COL + 2, s_win1);
    print_at(ENDING_ROW + 2, ENDING_COL + 2, s_win2);
    print_at(ENDING_ROW + 4, ENDING_COL + 1, s_win3);
    print_num(NLEVELS);
    print_at(ENDING_ROW + 5, ENDING_COL + 1, s_win4);
    print_at(ENDING_ROW + 7, ENDING_COL + 1, s_win5);
    cur_fg = CLR_LIT;
    print_num(total_stars());
    cur_fg = CLR_TEXT;
    put_code('/');
    print_num(NLEVELS * 3);
    print_at(ENDING_ROW + 8, ENDING_COL + 1, words->win6);
    cur_bg = COLOR_BLACK;               /* back to text on nothing */
    cur_fg = CLR_TEXT;
}

/* --- moves ------------------------------------------------------------------------ */

/* Rotate ring x by d (+1 clockwise, -1 counter-clockwise) */
static void rotate(int x, int d)
{
    int v = off[x] + d;
    if (v < 0)
        v = CELLS - 1;
    if (v == CELLS)
        v = 0;
    off[x] = (uint8_t)v;
}

/* Turn ring sel by d, along with every ring its gimbals drag.
 * Must match Level.turn in tools/level.py. */
static void turn_sel(int d)
{
    uint8_t same = lvl[LV_SAME + sel], opp = lvl[LV_OPP + sel];
    int snd = SND_COUNTER;              /* the sound says what the gimbals drag */
    if (!opp)
        snd = same == 1 << sel ? SND_TURN : SND_FOLLOW;
    play_sound(snd);
    for (int x = 0; x < RINGS; x++) {
        if (same >> x & 1)
            rotate(x, d);
        if (opp >> x & 1)
            rotate(x, -d);
    }
}

/* Turn the selected ring (and its gimbal followers) by d, recording the move */
static void do_move(int d)
{
    turn_sel(d);
    if (hist_n != 0xff)                 /* history: ring | $80 if counter-clockwise */
        hist[hist_n++] = (uint8_t)(sel | (d < 0 ? 0x80 : 0));
    if (moves != 0xff)
        moves++;
}

static void undo_move(void)
{
    if (!hist_n)
        return;
    uint8_t h = hist[--hist_n];
    sel = h & (RINGS - 1);
    turn_sel(h & 0x80 ? 1 : -1);
    if (moves)
        moves--;
}

/* scr[r*12+p] = cell shown at screen position p of ring r */
static void compute_scr(void)
{
    for (int r = 0; r < RINGS; r++) {
        int i = (CELLS - off[r]) % CELLS;   /* first cell shown = (-off) mod 12 */
        for (int p = 0; p < CELLS; p++) {
            scr[r * CELLS + p] = lvl[LV_CELLS + r * CELLS + i];
            i = (i + 1) % CELLS;
        }
    }
}

/* Queue cell k with parent x if it hasn't been seen */
static void enqueue(int k, int x)
{
    if (vis[k])
        return;
    vis[k] = 1;
    parent[k] = (uint8_t)x;
    queue[q_tail++] = (uint8_t)k;
}

/* Breadth-first search from the entry: mark every reachable screen cell in
 * vis with its parent in parent, and set won/goal at the first ring 0 cell
 * that opens into the burrow (so goal ends a shortest path). Must match
 * Level.solved() in tools/level.py. */
static void flood(void)
{
    memset(vis, 0, sizeof vis);
    won = 0;
    if (!(scr[ENTRY_K] & OUT))
        return;                         /* entry closed: nothing reachable */
    vis[ENTRY_K] = 1;
    parent[ENTRY_K] = ENTRY_K;
    queue[0] = ENTRY_K;
    q_head = 0;
    q_tail = 1;
    while (q_head != q_tail) {
        int x = queue[q_head++];
        if (x < CELLS && (scr[x] & IN) && !won) {   /* ring 0 opening inward: the burrow */
            won = 1;
            goal = (uint8_t)x;
        }
        if (scr[x] & CW)                /* clockwise neighbor */
            enqueue(pos_of(x) == CELLS - 1 ? x - (CELLS - 1) : x + 1, x);
        int y = pos_of(x) >= 1 ? x - 1 : x + CELLS - 1;   /* counter-clockwise, if it opens to us */
        if (scr[y] & CW)
            enqueue(y, x);
        if (x >= CELLS && (scr[x] & IN) && (scr[x - CELLS] & OUT))
            enqueue(x - CELLS, x);
        if (x < NCELLS - CELLS && (scr[x] & OUT) && (scr[x + CELLS] & IN))
            enqueue(x + CELLS, x);
    }
}

/* --- drawing ------------------------------------------------------------------------- */

/* Copy n bytes of the theme from offset y into the palette at byte *at */
static void put_theme(int *at, int y, int n)
{
    vera_palette_write(*at, theme + y, n);
    *at += n;
}

/* Write the board's palette rows from the level's theme: each ring's stone
 * (selected, dragged by a gimbal, or plain), tunnel, lit tunnel and wall,
 * then the fixed row's burrow (home once the tove is in) and entry. */
static void update_palette(void)
{
    uint8_t same = lvl[LV_SAME + sel], opp = lvl[LV_OPP + sel];
    for (int x = 0; x < RINGS; x++) {
        uint8_t v = TH_STONE;
        if (opp >> x & 1)
            v = TH_COUNTER;
        if (same >> x & 1)
            v = TH_FOLLOW;
        ring_stone[x] = v;
    }
    ring_stone[sel] = TH_SEL;

    int at = ROW_STONE * PALETTE_ROW;
    for (int x = 0; x < RINGS; x++)
        put_theme(&at, ring_stone[x], PALETTE_ROW);
    put_theme(&at, TH_TUNNEL, PALETTE_ROW);     /* rows 5-7 follow on */
    put_theme(&at, TH_LIT, PALETTE_ROW);
    put_theme(&at, TH_WALL, TH_BURROW - TH_WALL);

    at = ROW_FIXED * PALETTE_ROW;
    put_theme(&at, arrived ? TH_HOME : TH_BURROW, TH_HOME - TH_BURROW);
    put_theme(&at, TH_ENTRY, TH_SIZE - TH_ENTRY);
}

/* Copy a piece's spans into the bitmap, ORing cls into each pixel */
static void draw_spans(const uint8_t *p, uint8_t cls)
{
    for (int len; (len = *p) != 0; p += 4 + len) {
        int addr = p[1] | p[2] << 8 | (p[3] & 1) << 16;
        for (int i = 0; i < len && addr + i < SCREEN_W * SCREEN_H; i++)
            vera.bitmap[addr + i] = p[4 + i] | cls;     /* no effect on the literal wall rows */
    }
}

static const uint8_t *cells_ptr(uint8_t bank, uint8_t lo, uint8_t hi)
{
    return gg_bin_cells + (bank - CELLS_BANK) * CELLS_BANK_SIZE + ((hi << 8 | lo) - 0xa000);
}

static void draw_piece(int a, uint8_t cls)
{
    draw_spans(cells_ptr(D_piece_bank[a], D_piece_lo[a], D_piece_hi[a]), cls);
}

static void draw_teeth(int a, uint8_t cls)
{
    draw_spans(cells_ptr(D_teeth_bank[a], D_teeth_lo[a], D_teeth_hi[a]), cls);
}

/* Draw screen cell x with key: its base piece, its gear teeth where a
 * gimbal links its ring across a gap, then a piece per opening */
static void draw_cell(int x, uint8_t key)
{
    uint8_t cls = key & KEY_CLASS;
    draw_piece(x, cls);
    int r = ring_of(x);
    uint8_t teeth = ring_class(r);      /* teeth are the ring's, in its stone color */
    if (r != 0 && lvl[LV_TEETH + r - 1])        /* gimbal across the gap inside this ring */
        draw_teeth(x, teeth);
    if (r != RINGS - 1 && lvl[LV_TEETH + r])    /* ... and across the gap outside it */
        draw_teeth(x + NCELLS, teeth);
    for (int kind = 1; kind <= 4; kind++)       /* IN, OUT, CW, CCW */
        if (key >> (kind - 1) & 1)
            draw_piece(kind * NCELLS + x, cls);
}

/* Redraw every screen cell whose contents or color class changed. A cell's
 * key is its class row << 4 plus its openings (IN, OUT, CW, and CCW when its
 * counter-clockwise neighbor opens clockwise into it). */
static void draw_cells(void)
{
    for (int x = 0; x < NCELLS; x++) {
        uint8_t key;
        if (scr[x] & TUN) {
            key = scr[x] & (IN | OUT | CW);
            if (scr[ccw_of(x)] & CW)
                key |= CCW;
            key |= (vis[x] ? ROW_LIT : ROW_TUNNEL) << 4;
        } else {
            key = ring_class(ring_of(x));   /* stone: its ring's row, no openings */
        }
        if (key != drawn[x]) {
            drawn[x] = key;
            draw_cell(x, key);
        }
    }
}

/* Draw three stars at row, col, the first lit of them lit: 2x2 characters
 * each if big, else one character every other column */
static void draw_stars(int lit, int row, int col, bool big)
{
    for (int n = 0; n < 3; n++, col += 2) {
        plot(row, col);
        cur_fg = lit > n ? CLR_LIT : CLR_DIM;
        if (big) {
            put_code(STAR_SCREEN + STAR_BIG);
            put_code(STAR_SCREEN + STAR_BIG + 1);
            plot(row + 1, col);
            put_code(STAR_SCREEN + STAR_BIG + 2);
            put_code(STAR_SCREEN + STAR_BIG + 3);
        } else {
            put_code(STAR_SCREEN + STAR_SMALL);
        }
    }
    cur_fg = CLR_TEXT;
}

/* Copy the star glyphs over unused graphics characters of the charset */
static void init_stars(void)
{
    memcpy(&vera.charset[STAR_SCREEN * 8], D_star_chars, STAR_CHARS * 8);
}

static void draw_hud(void)
{
    clear_screen();
    print_at(SAFE_T, SAFE_L, s_title1);
    print_at(SAFE_T + 1, SAFE_L, s_title2);
    print_at(SAFE_T, SAFE_R - 8, s_moves);
    print_at(SAFE_T + 1, SAFE_R - 8, s_par);
    print_dec3(lvl[LV_PAR]);
    print_at(SAFE_T + 2, SAFE_L, s_level);  /* up a row: "Level 100" would */
    print_num(level + 1);                   /* reach the rim lower down */
    print_at(SAFE_T + 3, SAFE_L, s_best);
    print_at(SAFE_B - 3, SAFE_L, words->help1);
    print_at(SAFE_B - 2, SAFE_L, words->help2);
    print_at(SAFE_B - 1, SAFE_L, words->help3);
    print_at(SAFE_B, SAFE_L, words->help4);
    print_at(SAFE_B, SAFE_R - 5, words->help5);
}

static void draw_status(void)
{
    plot(SAFE_T, SAFE_R - 2);
    print_dec3(moves);
    draw_stars(best[level], SAFE_T + 4, SAFE_L, false);    /* best so far, small */
    if (arrived) {
        print_at(SAFE_T + 2, SAFE_R - 7, s_home1);
        print_at(SAFE_T + 3, SAFE_R - 7, s_home2);
        draw_stars(stars, SAFE_T + 4, SAFE_R - 5, true);    /* this solve, big */
        if (level < NLEVELS - 1) {
            print_at(SAFE_B - 2, SAFE_R - 7, words->next1);
            print_at(SAFE_B - 1, SAFE_R - 7, words->next2);
        } else {
            print_at(SAFE_B - 2, SAFE_R - 7, words->end1);
            print_at(SAFE_B - 1, SAFE_R - 7, words->end2);
        }
    } else {
        for (int row = SAFE_T + 2; row <= SAFE_T + 5; row++)
            print_at(row, SAFE_R - 7, s_blank);
        print_at(SAFE_B - 2, SAFE_R - 7, s_blank);
        print_at(SAFE_B - 1, SAFE_R - 7, s_blank);
    }
}

/* --- levels ---------------------------------------------------------------------------- */

/* Copy the current level's record into lvl and start it */
static void reset_level(void)
{
    memcpy(lvl, gg_inc_level + (D_level_lo[level] | D_level_hi[level] << 8), LEVEL_SIZE);
    for (int x = 0; x < RINGS; x++)
        off[x] = lvl[LV_START + x];
    moves = 0;
    hist_n = 0;
    sel = RINGS - 1;
    int t = lvl[LV_THEME];
    theme = gg_inc_themes + (D_theme_lo[t] | D_theme_hi[t] << 8);
    load_garden();
    want_music();
    memset(drawn, 0xff, sizeof drawn);  /* nothing drawn yet: draw every cell */
    draw_hud();
    refresh();
}

static void refresh(void)
{
    compute_scr();
    flood();
    if (!won) {
        walking = 0;                    /* not (or no longer) solved: tove waits at the entry */
        arrived = 0;
        want_music();                   /* ... to the theme's tune */
        tove_idle();
    } else if (!(walking | arrived)) {
        start_walk();                   /* just solved: walk home */
    }
    update_palette();
    draw_cells();
    draw_status();
}

static void goto_level(int n)
{
    if (n != level)
        progress_changed = true;
    level = (uint8_t)n;
    reset_level();
}

/* --- input --------------------------------------------------------------------------- */

static void toggle_music(void)
{
    music_off ^= 1;
    progress_changed = true;
    want_music();
}

/* Act on key k. True to quit. */
static bool handle_key(int k)
{
    if (k >= 'A' && k <= 'Z')           /* fold shifted letters (caps lock) */
        k += 'a' - 'A';
    if (finished) {
        if (k == KEY_RETURN) {          /* the ending: Return plays again from level 1 */
            finished = 0;
            goto_level(0);
        } else if (k == 'm') {
            toggle_music();
        } else if (k == 'q') {
            return true;
        }
        return false;
    }
    switch (k) {
    case KEY_UP:
    case 'w':
        if (sel != RINGS - 1) {
            sel++;
            refresh();
        }
        break;
    case KEY_DOWN:
    case 's':
        if (sel) {
            sel--;
            refresh();
        }
        break;
    case KEY_LEFT:
    case 'a':
    case KEY_RIGHT:
    case 'd':
        if (!won) {
            do_move(k == KEY_LEFT || k == 'a' ? -1 : 1);
            refresh();
        }
        break;
    case 'u':
    case KEY_DEL:
        undo_move();
        refresh();
        break;
    case 'r':
        reset_level();
        break;
    case KEY_RETURN:
        if (!won)
            break;
        if (!arrived) {
            arrive();                   /* still walking: skip to the end */
            break;
        }
        /* fall through */
    case 'n':
        if (level + 1 < NLEVELS)
            goto_level(level + 1);
        else if (arrived)               /* past the last level: the ending */
            show_ending();
        break;
    case 'p':
        if (level)
            goto_level(level - 1);
        break;
    case 'm':
        toggle_music();
        break;
    case 'q':
        return true;
    }
    return false;
}

bool game_key(int key)
{
    return handle_key(key);
}

void game_select_ring(int r)
{
    if (finished || r < 0 || r >= RINGS || r == sel)
        return;
    sel = (uint8_t)r;
    refresh();
}

/* Key code for each pad button, in bit order from bit 7 down (0 = unused) */
static const uint8_t joy_keys0[8] = {KEY_DEL, 0, 'r', KEY_RETURN, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
static const uint8_t joy_keys1[4] = {KEY_RETURN, 0, 0, 0};

/* The pads' new presses, translated into the same key codes as the
 * keyboard, so handle_key is the only place game input is handled. A held
 * D-pad direction repeats. */
static bool poll_joy(uint8_t joy0, uint8_t joy1)
{
    uint8_t new0 = (joy0 ^ joy0_prev) & joy0;
    uint8_t new1 = (joy1 ^ joy1_prev) & joy1;
    joy0_prev = joy0;
    joy1_prev = joy1;

    if (new0 & JOY_DPAD) {
        joy_timer = JOY_DELAY;
    } else if (joy0 & JOY_DPAD) {
        if (--joy_timer == 0) {
            joy_timer = JOY_RATE;
            new0 |= joy0 & JOY_DPAD;
        }
    }
    for (int x = 0; x < 8; x++)
        if (new0 & (0x80 >> x) && joy_keys0[x] && handle_key(joy_keys0[x]))
            return true;
    for (int x = 0; x < 4; x++)
        if (new1 & (0x80 >> x) && joy_keys1[x] && handle_key(joy_keys1[x]))
            return true;
    return false;
}

bool game_tick(uint8_t joy0, uint8_t joy1)
{
    if (poll_joy(joy0, joy1))
        return true;
    tick_tove();
    tick_critters();
    return false;
}

/* --- setup ---------------------------------------------------------------------------- */

/* Zero every variable, as clear_bss does: nothing may rely on the state a
 * previous run left behind */
static void clear_state(void)
{
    finished = music_off = critter_z = 0;
    memset(cr_x, 0, sizeof cr_x);
    memset(cr_y, 0, sizeof cr_y);
    memset(cr_tx, 0, sizeof cr_tx);
    memset(cr_ty, 0, sizeof cr_ty);
    memset(cr_walk, 0, sizeof cr_walk);
    memset(cr_timer, 0, sizeof cr_timer);
    memset(cr_flip, 0, sizeof cr_flip);
    memset(parent, 0, sizeof parent);
    memset(queue, 0, sizeof queue);
    q_head = q_tail = goal = path_n = prev_k = wp_n = wp_i = 0;
    memset(path, 0, sizeof path);
    memset(wp_x, 0, sizeof wp_x);
    memset(wp_y, 0, sizeof wp_y);
    walking = arrived = seg_count = anim = 0;
    posx = posy = stepx = stepy = 0;
    tove_x = tove_y = tove_frame = tove_flip = tove_z = 0;
    joy0_prev = joy1_prev = joy_timer = 0;
    level = sel = moves = won = stars = hist_n = 0;
    memset(off, 0, sizeof off);
    memset(best, 0, sizeof best);
    progress_changed = false;
}

void game_init(uint32_t rnd, HelpStyle help)
{
    clear_state();
    words = help == HELP_TOUCH ? &touch_words : &keyboard_words;
    cur_fg = CLR_TEXT;
    cur_bg = COLOR_BLACK;
    memcpy(vera.bitmap, gg_bin_rings, sizeof vera.bitmap);
    init_stars();
    init_tove();
    init_critters(rnd);
    reset_level();
}

int game_selected_ring(void) { return sel; }
bool game_can_advance(void) { return finished || won; }
bool game_can_undo(void) { return !finished && hist_n > 0; }
bool game_finished(void) { return finished; }
bool game_music_off(void) { return music_off; }
bool game_progress_changed(void) { return progress_changed; }

/* Saved progress: "GG1", level, music_off, then best[NLEVELS] */
#define SAVE_SIZE (5 + NLEVELS)

int game_save(uint8_t *buf, int size)
{
    if (size < SAVE_SIZE)
        return 0;
    memcpy(buf, "GG1", 3);
    buf[3] = level;
    buf[4] = music_off;
    memcpy(buf + 5, best, NLEVELS);
    progress_changed = false;
    return SAVE_SIZE;
}

void game_load(const uint8_t *buf, int size)
{
    if (size != SAVE_SIZE || memcmp(buf, "GG1", 3) || buf[3] >= NLEVELS)
        return;
    music_off = buf[4] & 1;
    for (int i = 0; i < NLEVELS; i++)
        best[i] = buf[5 + i] <= 3 ? buf[5 + i] : 0;
    finished = 0;
    level = buf[3];
    reset_level();
    progress_changed = false;
}
