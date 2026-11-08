/* Gyre & Gimble: the game logic, ported from the X16's src/main.asm */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

/* Key codes, as the X16's GETIN returns them (letters are lowercase ASCII) */
#define KEY_UP 0x91
#define KEY_DOWN 0x11
#define KEY_LEFT 0x9d
#define KEY_RIGHT 0x1d
#define KEY_DEL 0x14
#define KEY_RETURN 0x0d

/* SNES pad buttons, as the X16's joystick_get reports them (but 1 = down),
 * merged over every pad. Byte 0: */
#define JOY_B 0x80
#define JOY_Y 0x40
#define JOY_SELECT 0x20
#define JOY_START 0x10
#define JOY_UP 0x08
#define JOY_DOWN 0x04
#define JOY_LEFT 0x02
#define JOY_RIGHT 0x01
/* byte 1: */
#define JOY_A 0x80
#define JOY_X 0x40
#define JOY_L 0x20
#define JOY_R 0x10

/* Which help text the HUD shows */
typedef enum { HELP_KEYBOARD, HELP_TOUCH } HelpStyle;

void game_init(uint32_t seed, HelpStyle help);

/* Act on a key; true to quit */
bool game_key(int key);

/* Once per frame (60 Hz): the pads' state, then the tove and critters.
 * True to quit. */
bool game_tick(uint8_t joy0, uint8_t joy1);

/* Touch: select ring r (0 = innermost), as W/S would step to it */
void game_select_ring(int r);

/* State for the on-screen buttons */
int game_selected_ring(void);
bool game_can_advance(void);    /* Return would do something */
bool game_can_undo(void);
bool game_finished(void);       /* showing the ending */
bool game_music_off(void);

/* Saved progress: the level, music setting and each level's best stars */
int game_save(uint8_t *buf, int size);
void game_load(const uint8_t *buf, int size);
bool game_progress_changed(void);  /* since the last game_save */

#endif
