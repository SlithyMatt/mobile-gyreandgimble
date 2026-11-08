#include "touch.h"

#include <math.h>

#include "game.h"
#include "gg_data.h"

#define STEP_DEG (360.0f / GEO_CELLS)
#define HYSTERESIS_DEG 3.0f     /* past the halfway point before a step */
#define SWIPE_MIN 14.0f         /* game pixels before a swipe counts */
#define RIM_SLOP 14.0f          /* the outer ring reaches past the rim, for fingers */
#define CENTER_DEAD 6.0f        /* too near the center to read an angle */

enum { IDLE, TURNING, SWIPING };

static int mode = IDLE;
static float start_x, start_y;            /* where the touch began */
static float last_angle;        /* degrees, clockwise from the top */
static float travel;            /* degrees turned since the touch began */
static int steps;               /* moves made by this touch (+ clockwise) */

static float dist(float x, float y)
{
    return hypotf(x - GEO_CX, y - GEO_CY);
}

static float angle(float x, float y)
{
    return atan2f(x - GEO_CX, -(y - GEO_CY)) * 180.0f / 3.14159265f;
}

int touch_ring_at(float x, float y)
{
    float d = dist(x, y);
    float inner = GEO_RING_IN0 - GEO_GAP_W / 2.0f;      /* ring 0's band starts here */
    if (d < GEO_BURROW_R || d >= GEO_RIM_OUT + RIM_SLOP)
        return -1;
    int r = (int)((d - inner) / GEO_RING_PITCH);
    return r < 0 ? 0 : r >= GEO_RINGS ? GEO_RINGS - 1 : r;
}

void touch_down(float x, float y)
{
    start_x = x;
    start_y = y;
    int r = touch_ring_at(x, y);
    if (r >= 0) {
        mode = TURNING;
        game_select_ring(r);
        last_angle = angle(x, y);
        travel = 0;
        steps = 0;
    } else {
        mode = SWIPING;
    }
}

void touch_move(float x, float y)
{
    if (mode == TURNING) {
        if (dist(x, y) < CENTER_DEAD)
            return;
        float a = angle(x, y);
        float d = a - last_angle;
        while (d > 180)
            d -= 360;
        while (d <= -180)
            d += 360;
        last_angle = a;
        travel += d;
        /* the ring follows the finger, to the nearest step */
        while (travel > (steps + 0.5f) * STEP_DEG + HYSTERESIS_DEG) {
            steps++;
            game_key(KEY_RIGHT);
        }
        while (travel < (steps - 0.5f) * STEP_DEG - HYSTERESIS_DEG) {
            steps--;
            game_key(KEY_LEFT);
        }
    } else if (mode == SWIPING) {
        float dx = x - start_x, dy = y - start_y;
        if (fabsf(dx) < SWIPE_MIN && fabsf(dy) < SWIPE_MIN)
            return;
        if (fabsf(dx) > fabsf(dy))
            game_key(dx > 0 ? KEY_RIGHT : KEY_LEFT);
        else
            game_key(dy < 0 ? KEY_UP : KEY_DOWN);
        mode = IDLE;            /* one step per swipe */
    }
}

void touch_up(float x, float y)
{
    touch_move(x, y);
    mode = IDLE;
}

void touch_cancel(void)
{
    mode = IDLE;
}
