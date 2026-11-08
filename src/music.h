/* The background music sequencer (main.asm's music_irq and friends). It
 * runs on the audio thread, once per VSYNC; the game only names the tune it
 * wants (holding the audio lock). */
#ifndef MUSIC_H
#define MUSIC_H

#include <stdbool.h>
#include <stdint.h>

#define NO_TUNE 0xff

void music_init(bool has_ym);
void music_tick(void);          /* the VSYNC hook */

/* Call with the audio lock held */
void music_want(uint8_t tune);
void music_again(void);         /* restart the wanted tune, even if it's playing */
void music_silence(void);

#endif
