/* The X16's sound chips: VERA's PSG (16 voices) and PCM FIFO, and the
 * YM2151 (via ymfm), mixed as the X16 mixes them.
 *
 * audio_render() runs on the platform's audio thread. Every 1/60 s of
 * output it calls the VSYNC hook first, as the X16 runs the game's music
 * from its VSYNC interrupt. Code on any other thread must hold the audio
 * lock (audio_lock/audio_unlock) while it touches the chips. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void audio_init(int host_rate, void (*vsync)(void));

/* Fill frames stereo int16 frames */
void audio_render(int16_t *out, int frames);

/* Set by the platform: lock and unlock the audio thread out */
void audio_set_lock(void (*lock)(void), void (*unlock)(void));
void audio_lock(void);
void audio_unlock(void);

/* VERA PSG: register reg (0-63) of the 16 voices x 4 registers */
void psg_write(int reg, uint8_t val);

/* VERA PCM: AUDIO_CTRL, AUDIO_RATE, AUDIO_DATA */
void pcm_write_ctrl(uint8_t val);
void pcm_write_rate(uint8_t val);
void pcm_write_fifo(uint8_t val);
bool pcm_fifo_empty(void);

/* YM2151 */
void ym_write_reg(uint8_t reg, uint8_t val);

#endif
