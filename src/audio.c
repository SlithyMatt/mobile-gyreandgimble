/* PSG, PCM and mixer behavior follow the X16 emulator's vera_psg.c,
 * vera_pcm.c and audio.c (x16emu, 2-clause BSD; the volume tables are
 * from there), so the game sounds as it does in x16emu. */
#include "audio.h"

#include <string.h>

#include "ym.h"

#define VERA_RATE 48828.125         /* 25 MHz / 512 */
#define YM_RATE (3579545.0 / 64)    /* the YM2151's sample rate */
#define VSYNC_HZ 60

/* --- PSG ------------------------------------------------------------- */

enum { WF_PULSE, WF_SAWTOOTH, WF_TRIANGLE, WF_NOISE };

typedef struct {
    uint16_t freq, volume;
    bool left, right;
    uint8_t pw, waveform;
    uint16_t noiseval;
    uint32_t phase;
} Voice;

static Voice voices[16];
static uint16_t noise_state = 1;

static const uint16_t psg_volume_lut[64] = {
    0, 4, 8, 12, 16, 17, 18, 20, 21, 22, 23, 25, 26, 28, 30, 31,
    33, 35, 37, 40, 42, 45, 47, 50, 53, 56, 60, 63, 67, 71, 75, 80,
    85, 90, 95, 101, 107, 113, 120, 127, 135, 143, 151, 160, 170, 180, 191, 202,
    214, 227, 241, 255, 270, 286, 303, 321, 341, 361, 382, 405, 429, 455, 482, 511,
};

void psg_write(int reg, uint8_t val)
{
    Voice *v = &voices[(reg & 0x3f) / 4];
    switch (reg & 3) {
    case 0: v->freq = (v->freq & 0xff00) | val; break;
    case 1: v->freq = (v->freq & 0x00ff) | val << 8; break;
    case 2:
        v->right = val & 0x80;
        v->left = val & 0x40;
        v->volume = psg_volume_lut[val & 0x3f];
        break;
    case 3:
        v->pw = val & 0x3f;
        v->waveform = val >> 6;
        break;
    }
}

static void psg_sample(int *l, int *r)
{
    *l = *r = 0;
    for (int i = 0; i < 16; i++) {
        noise_state = (uint16_t)(noise_state << 1 | (((noise_state >> 1) ^ (noise_state >> 2) ^
                                                      (noise_state >> 4) ^ (noise_state >> 15)) & 1));
        Voice *v = &voices[i];
        uint32_t phase = (v->left || v->right) ? (v->phase + v->freq) & 0x1ffff : 0;
        if ((v->phase & 0x10000) && !(phase & 0x10000))
            v->noiseval = (noise_state >> 1) & 0x3f;
        v->phase = phase;

        uint32_t s = 0;
        switch (v->waveform) {
        case WF_PULSE: s = (v->phase >> 10) > v->pw ? 0 : 0x3f; break;
        case WF_SAWTOOTH: s = (v->phase >> 11) ^ ((v->pw ^ 0x3f) & 0x3f); break;
        case WF_TRIANGLE:
            s = ((v->phase & 0x10000) ? (~(v->phase >> 10) & 0x3f) : ((v->phase >> 10) & 0x3f)) ^
                ((v->pw ^ 0x3f) & 0x3f);
            break;
        case WF_NOISE: s = v->noiseval; break;
        }
        int sv = (int)((s ^ 0x20) & 0x3f);
        if (sv & 0x20)
            sv -= 0x40;                     /* 6-bit signed */
        int out = sv * v->volume;
        if (v->left)
            *l += out >> 3;
        if (v->right)
            *r += out >> 3;
    }
}

/* --- PCM ------------------------------------------------------------- */

static uint8_t fifo[4096];
static unsigned fifo_wr, fifo_rd, fifo_cnt;
static uint8_t pcm_ctrl, pcm_rate, pcm_phase;
static int pcm_cur;

static const uint8_t pcm_volume_lut[16] = {0, 1, 2, 3, 4, 5, 6, 8, 11, 14, 18, 23, 30, 38, 49, 64};

void pcm_write_ctrl(uint8_t val)
{
    if (val & 0x80)
        fifo_wr = fifo_rd = fifo_cnt = 0;
    pcm_ctrl = val & 0x3f;
}

void pcm_write_rate(uint8_t val)
{
    pcm_rate = val > 128 ? 256 - val : val;
}

void pcm_write_fifo(uint8_t val)
{
    if (fifo_cnt < sizeof fifo - 1) {
        fifo[fifo_wr] = val;
        fifo_wr = (fifo_wr + 1) % sizeof fifo;
        fifo_cnt++;
    }
}

bool pcm_fifo_empty(void)
{
    return fifo_cnt == 0;
}

static int pcm_sample(void)
{
    uint8_t old = pcm_phase;
    pcm_phase += pcm_rate;
    if ((old ^ pcm_phase) & 0x80) {
        if (fifo_cnt == 0) {
            pcm_cur = 0;
        } else {    /* the game only plays 8-bit mono */
            pcm_cur = (int8_t)fifo[fifo_rd] * 256;
            fifo_rd = (fifo_rd + 1) % sizeof fifo;
            fifo_cnt--;
        }
    }
    return pcm_cur * pcm_volume_lut[pcm_ctrl & 0x0f] / 64;
}

/* --- mixing ---------------------------------------------------------- */

static int host_rate = 48000;
static void (*vsync_hook)(void);
static void (*lock_fn)(void), (*unlock_fn)(void);

static double vera_step, ym_step;       /* native samples per host sample */
static double vera_pos, ym_pos;         /* fraction of the way to the next one */
static int vera_prev[2], vera_next[2], ym_prev[2], ym_next[2];
static int vsync_left;                  /* host samples until the next VSYNC */
static int limiter = 1 << 16;

void audio_init(int rate, void (*vsync)(void))
{
    memset(voices, 0, sizeof voices);
    noise_state = 1;
    fifo_wr = fifo_rd = fifo_cnt = 0;
    pcm_ctrl = pcm_rate = pcm_phase = 0;
    pcm_cur = 0;
    host_rate = rate;
    vsync_hook = vsync;
    vera_step = VERA_RATE / rate;
    ym_step = YM_RATE / rate;
    vera_pos = ym_pos = 0;
    memset(vera_prev, 0, sizeof vera_prev);
    memset(vera_next, 0, sizeof vera_next);
    memset(ym_prev, 0, sizeof ym_prev);
    memset(ym_next, 0, sizeof ym_next);
    vsync_left = 0;
    limiter = 1 << 16;
    ym_reset();
}

void audio_set_lock(void (*lock)(void), void (*unlock)(void))
{
    lock_fn = lock;
    unlock_fn = unlock;
}

void audio_lock(void)
{
    if (lock_fn)
        lock_fn();
}

void audio_unlock(void)
{
    if (unlock_fn)
        unlock_fn();
}

void ym_write_reg(uint8_t reg, uint8_t val)
{
    ym_write(reg, val);
}

static void vera_advance(void)
{
    int l, r, p = pcm_sample();
    psg_sample(&l, &r);
    vera_prev[0] = vera_next[0];
    vera_prev[1] = vera_next[1];
    vera_next[0] = l + p;
    vera_next[1] = r + p;
}

static void ym_advance(void)
{
    int16_t lr[2];
    ym_generate(lr);
    ym_prev[0] = ym_next[0];
    ym_prev[1] = ym_next[1];
    ym_next[0] = lr[0];
    ym_next[1] = lr[1];
}

void audio_render(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        if (vsync_left <= 0) {
            vsync_left += host_rate / VSYNC_HZ;
            if (vsync_hook)
                vsync_hook();
        }
        vsync_left--;

        for (vera_pos += vera_step; vera_pos >= 1; vera_pos -= 1)
            vera_advance();
        for (ym_pos += ym_step; ym_pos >= 1; ym_pos -= 1)
            ym_advance();

        /* As on the X16: (PSG + PCM) * 2 + YM, with a limiter */
        int mix[2];
        for (int c = 0; c < 2; c++) {
            double v = vera_prev[c] + (vera_next[c] - vera_prev[c]) * vera_pos;
            double y = ym_prev[c] + (ym_next[c] - ym_prev[c]) * ym_pos;
            mix[c] = (int)(v * 2 + y);
        }
        int amp = mix[0] < 0 ? -mix[0] : mix[0];
        int amp_r = mix[1] < 0 ? -mix[1] : mix[1];
        if (amp_r > amp)
            amp = amp_r;
        if (amp > 32767) {
            int lim = (int)((32767LL << 16) / amp);
            if (lim < limiter)
                limiter = lim;
        }
        out[i * 2] = (int16_t)(((long long)mix[0] * limiter) >> 16);
        out[i * 2 + 1] = (int16_t)(((long long)mix[1] * limiter) >> 16);
        if (limiter < (1 << 16))
            limiter++;
    }
}
