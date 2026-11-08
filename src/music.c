/* Port of the background music in main.asm: each theme's tune is a set of
 * channels, each a loop of (note, frames) pairs for one PSG voice or YM2151
 * channel, compiled by the X16 repo's tools/genmusic.py. Every VSYNC steps
 * every channel once. */
#include "music.h"

#include "audio.h"
#include "gg_data.h"

static uint8_t has_ym;
static uint8_t want = NO_TUNE, cur = NO_TUNE, again;
static uint8_t music_n;
static uint8_t ch_hw[MUSIC_CHANNELS];       /* bit 7 = YM; PSG voice or YM channel */
static uint8_t ch_instr[MUSIC_CHANNELS];
static uint16_t ch_start[MUSIC_CHANNELS];   /* offsets into the music blob */
static uint16_t ch_ptr[MUSIC_CHANNELS];
static uint8_t ch_left[MUSIC_CHANNELS];     /* frames until the next pair */
static uint8_t ch_vol[MUSIC_CHANNELS];      /* PSG volume, 8.8 */
static uint8_t ch_vol_lo[MUSIC_CHANNELS];

#define MUSIC gg_inc_music

void music_init(bool ym)
{
    has_ym = ym;
    want = cur = NO_TUNE;
    again = 0;
    music_n = 0;
}

void music_want(uint8_t tune)
{
    want = tune;
}

void music_again(void)
{
    again = 1;
}

static void ym_put(uint8_t reg, uint8_t val)
{
    if (has_ym)
        ym_write_reg(reg, val);
}

void music_silence(void)
{
    for (int r = MUSIC_PSG_VOICES * 4 - 1; r >= 0; r--)
        psg_write(r, 0);
    if (has_ym)
        for (int c = 7; c >= 0; c--)
            ym_put(0x08, (uint8_t)c);   /* key off */
}

static void psg_reg(int x, int reg, uint8_t val)
{
    psg_write(ch_hw[x] * 4 + reg, val);
}

static void psg_strike(int x, uint8_t note)
{
    uint8_t in = ch_instr[x];
    psg_reg(x, 0, D_psg_freq_lo[note]);
    psg_reg(x, 1, D_psg_freq_hi[note]);
    ch_vol[x] = D_psg_peak[in];
    ch_vol_lo[x] = 0;
    psg_reg(x, 2, ch_vol[x] | 0xc0);    /* both speakers */
    psg_reg(x, 3, D_psg_wave[in]);
}

/* Decay channel x's volume toward its sustain level, and write it */
static void psg_envelope(int x)
{
    uint8_t in = ch_instr[x];
    uint8_t sustain = D_psg_sustain[in];
    if (ch_vol[x] > sustain) {
        int v = (ch_vol[x] << 8 | ch_vol_lo[x]) - (D_psg_decay_hi[in] << 8 | D_psg_decay_lo[in]);
        if (v < 0 || (v >> 8) < sustain) {
            ch_vol[x] = sustain;            /* ch_vol_lo keeps the subtraction's low byte */
            ch_vol_lo[x] = (uint8_t)v;
        } else {
            ch_vol[x] = (uint8_t)(v >> 8);
            ch_vol_lo[x] = (uint8_t)v;
        }
    }
    psg_reg(x, 2, ch_vol[x] | 0xc0);
}

static void ym_release(int x)
{
    ym_put(0x08, ch_hw[x] & 7);
}

static void ym_strike(int x, uint8_t note)
{
    ym_release(x);                      /* key off, so the envelope starts again */
    ym_put(0x28 + (ch_hw[x] & 7), D_ym_kc[note]);
    ym_put(0x08, 0x78 | (ch_hw[x] & 7)); /* key on, all four operators */
}

/* Load channel x's instrument patch into its YM channel */
static void ym_patch(int x)
{
    uint8_t in = ch_instr[x];
    const uint8_t *p = MUSIC + (D_ym_patch_lo[in] | D_ym_patch_hi[in] << 8);
    uint8_t c = ch_hw[x] & 7;
    ym_put(0x20 + c, p[0]);             /* RL/FB/CONNECT */
    ym_put(0x38 + c, p[1]);             /* PMS/AMS */
    uint8_t reg = 0x40 + c;             /* 6 registers x 4 operators, 8 apart */
    for (int y = 2; y < 26; y++, reg += 8)
        ym_put(reg, p[y]);
}

/* Silence the old tune and set up the wanted one's channels */
static void start_tune(void)
{
    music_silence();
    cur = want;
    if (cur == NO_TUNE)
        return;
    const uint8_t *t = MUSIC + (D_tune_lo[cur] | D_tune_hi[cur] << 8);
    music_n = *t++;
    for (int x = 0; x < music_n; x++, t += 4) {
        ch_hw[x] = t[0];
        ch_instr[x] = t[1];
        ch_start[x] = ch_ptr[x] = (uint16_t)(t[2] | t[3] << 8);
        ch_left[x] = 1;                 /* first note on the next step */
        ch_vol[x] = 0;
    }
    if (has_ym)
        for (int x = 0; x < music_n; x++)
            if (ch_hw[x] & 0x80)
                ym_patch(x);
}

/* Channel x's next (note, frames) pair: strike, hold or rest */
static void next_note(int x)
{
    const uint8_t *p = MUSIC + ch_ptr[x];
    uint8_t note = p[0];
    if (note == MUSIC_STOP) {           /* the end of a tune that plays once: */
        ch_left[x] = 0xff;              /* rest here for good */
        note = 0;
    } else {
        if (note == MUSIC_LOOP) {
            p = MUSIC + ch_start[x];
            note = p[0];
        }
        ch_left[x] = p[1];
        ch_ptr[x] = (uint16_t)(p + 2 - MUSIC);
    }
    if (note & MUSIC_TIE)
        return;                         /* held over */
    if (ch_hw[x] & 0x80) {
        if (note)
            ym_strike(x, note);
        else
            ym_release(x);
    } else {
        if (note)
            psg_strike(x, note);
        else
            ch_vol[x] = 0;
    }
}

void music_tick(void)
{
    if (again) {
        again = 0;
        start_tune();
    } else if (want != cur) {
        start_tune();
    }
    if (cur == NO_TUNE)
        return;
    for (int x = 0; x < music_n; x++) {
        if ((ch_hw[x] & 0x80) && !has_ym)
            continue;
        if (--ch_left[x] == 0)
            next_note(x);
        if (!(ch_hw[x] & 0x80))
            psg_envelope(x);
    }
}
