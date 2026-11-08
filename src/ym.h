/* The YM2151, emulated by ymfm (third_party/ymfm, BSD-3) */
#ifndef YM_H
#define YM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ym_reset(void);
void ym_write(uint8_t reg, uint8_t val);
void ym_generate(int16_t lr[2]);    /* one sample at 3579545 / 64 Hz */

#ifdef __cplusplus
}
#endif

#endif
