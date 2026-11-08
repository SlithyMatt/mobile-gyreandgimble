/* Touchscreen gestures, in the game's 320x240 coordinates:
 *
 *  - Touch a ring to select it. Drag around the board's center to turn
 *    it: the ring follows the finger, a step (one move) every 30 degrees.
 *  - Swipe anywhere off the rings: up/down selects the next ring out/in,
 *    left/right turns the selected ring (as W/S and A/D do on the X16).
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>

void touch_down(float x, float y);
void touch_move(float x, float y);
void touch_up(float x, float y);
void touch_cancel(void);

/* The ring a touch at (x, y) is on, or -1 */
int touch_ring_at(float x, float y);

#endif
