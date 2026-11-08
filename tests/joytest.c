/* Headless test of the game core, using the X16 repo's own test script.
 *
 *   joytest JOYSCRIPT.inc LEVEL1_KEYS [SHOT_DIR]
 *
 * JOYSCRIPT.inc comes from x16/tools/joyscript.py: a scripted SNES pad that
 * plays every level's shortest solution (with hold-to-repeat, an undo and a
 * restart along the way) and ends on the ending screen, as `make joytest`
 * does on the X16. The test feeds it to game_tick() frame by frame, running
 * the music and sound as the platform would, then checks that the game
 * reached the ending with every level solved at par (300 stars).
 *
 * It then starts afresh and solves level 1 with touch gestures (touch.c),
 * from LEVEL1_KEYS, level 1's shortest solution as WASD keys.
 *
 * With SHOT_DIR, it writes frames as PPM files to compare with the X16
 * screenshots in x16/docs/. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "game.h"
#include "gg_data.h"
#include "music.h"
#include "touch.h"
#include "vera.h"

static uint32_t frame[SCREEN_W * SCREEN_H];
static const char *shot_dir;
static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
            failures++;                                        \
        }                                                      \
    } while (0)

static void shot(const char *name)
{
    if (!shot_dir)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", shot_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    vera_render(frame);
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t rgb[3] = {frame[i] >> 16 & 0xff, frame[i] >> 8 & 0xff, frame[i] & 0xff};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* One 60 Hz frame of everything: the game, then 1/60 s of audio (whose
 * VSYNC hook runs the music) */
static int16_t samples[800 * 2];
static double peak;

static bool frame_tick(uint8_t j0, uint8_t j1)
{
    bool quit = game_tick(j0, j1);
    audio_render(samples, 800);
    for (int i = 0; i < 1600; i++)
        if (abs(samples[i]) > peak)
            peak = abs(samples[i]);
    return quit;
}

static int total_stars(void)
{
    uint8_t buf[256];
    int n = game_save(buf, sizeof buf), sum = 0;
    for (int i = 5; i < n; i++)
        sum += buf[i];
    return sum;
}

static void run_joyscript(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
    }
    char line[256];
    int frames = 0;
    bool started = false;
    while (fgets(line, sizeof line, f)) {
        unsigned b0, b1, n;
        if (sscanf(line, " .byte $%x, $%x, %u", &b0, &b1, &n) == 3) {
            /* active-low, like JOYSTICK_GET */
            uint8_t j0 = (uint8_t)~b0, j1 = (uint8_t)(~b1 & 0xf0);
            for (unsigned i = 0; i < n; i++, frames++) {
                CHECK(!frame_tick(j0, j1), "quit during the script");
                if (frames == 5 && !started) {
                    shot("level1");
                    started = true;
                }
            }
        } else if (strstr(line, "$00") && strstr(line, "end")) {
            break;
        }
    }
    fclose(f);
    for (int i = 0; i < 120; i++)   /* let the fanfare start */
        frame_tick(0, 0);
    shot("ending");
    printf("joyscript: %d frames\n", frames);
    CHECK(game_finished(), "the script should end on the ending screen");
    CHECK(total_stars() == NLEVELS * 3, "every level solved at par: %d stars, want %d",
          total_stars(), NLEVELS * 3);
    CHECK(peak > 1000, "audio should play (peak %.0f)", peak);
}

/* Screen position of ring r at angle a (degrees clockwise from the top) */
static void ring_point(int r, float a, float *x, float *y)
{
    float d = GEO_RING_IN0 + r * GEO_RING_PITCH + GEO_RING_W / 2.0f;
    *x = GEO_CX + d * sinf(a * 3.14159265f / 180);
    *y = GEO_CY - d * cosf(a * 3.14159265f / 180);
}

/* Turn ring r by `steps` (+ clockwise) with one drag around the board */
static void drag_ring(int r, int steps)
{
    float a = 200, x, y;    /* start low on the ring, clear of the tove */
    ring_point(r, a, &x, &y);
    touch_down(x, y);
    CHECK(game_selected_ring() == r, "touching ring %d should select it", r);
    for (int i = 0; i < abs(steps) * 6; i++) {
        a += steps > 0 ? 5 : -5;
        ring_point(r, a, &x, &y);
        touch_move(x, y);
        frame_tick(0, 0);
    }
    touch_up(x, y);
}

/* Solve level 1 by touch, playing its shortest solution (WASD keys from
 * x16/tools/level.py keys) as gestures: W/S as a tap on the next ring out
 * or in, A/D as a drag one step around the board */
static void run_touch(const char *solution)
{
    vera_init();
    audio_init(48000, music_tick);
    music_init(true);
    game_init(1, HELP_TOUCH);
    CHECK(game_selected_ring() == 3, "the outer ring starts selected");

    /* swipes off the board: down selects the next ring in, up the next out */
    touch_down(20, 120);
    touch_move(20, 140);
    touch_up(20, 140);
    CHECK(game_selected_ring() == 2, "swiping down should select ring 2, not %d", game_selected_ring());
    touch_down(300, 140);
    touch_move(300, 120);
    touch_up(300, 120);
    CHECK(game_selected_ring() == 3, "swiping up should select ring 3, not %d", game_selected_ring());

    /* a drag two steps one way and back again turns the ring 4 times, and
     * leaves it where it was */
    drag_ring(3, 2);
    drag_ring(3, -2);
    CHECK(game_can_undo(), "drags should be undoable moves");
    for (int i = 0; i < 4; i++)
        game_key(KEY_DEL);
    CHECK(!game_can_undo(), "four drag steps should undo in four");

    int sel = 3;
    for (const char *k = solution; *k; k++) {
        if (*k == 'w' || *k == 's') {
            sel += *k == 'w' ? 1 : -1;
            float x, y;
            ring_point(sel, 90, &x, &y);
            touch_down(x, y);
            touch_up(x, y);
            CHECK(game_selected_ring() == sel, "tapping ring %d should select it", sel);
        } else {
            CHECK(!game_can_advance(), "solved early, at key %ld", (long)(k - solution));
            drag_ring(sel, *k == 'd' ? 1 : -1);
        }
    }
    CHECK(game_can_advance(), "the touch solution should solve level 1");
    for (int i = 0; i < 600; i++)       /* the tove walks home */
        frame_tick(0, 0);
    shot("touch_solved");
    CHECK(total_stars() == 3, "a par solve by touch earns 3 stars, not %d", total_stars());
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: joytest JOYSCRIPT.inc LEVEL1_KEYS [SHOT_DIR]\n");
        return 2;
    }
    shot_dir = argc > 3 ? argv[3] : NULL;
    vera_init();
    audio_init(48000, music_tick);
    music_init(true);
    game_init(1, HELP_KEYBOARD);
    run_joyscript(argv[1]);
    run_touch(argv[2]);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
