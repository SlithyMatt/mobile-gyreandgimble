/* A model of the parts of the X16's VERA video chip the game uses, in mode
 * $80: a 320x240 8bpp bitmap on layer 0, a 40x30 text layer on layer 1,
 * 16x16 4bpp sprites at Z-depth 2 (between the layers), and the 256-entry
 * 12-bit palette. The game writes these the way it writes VERA on the X16;
 * vera_render() composites them into an RGBA frame. */
#ifndef VERA_H
#define VERA_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define TEXT_COLS 40
#define TEXT_ROWS 30
#define NSPRITES 8

/* PETSCII color codes as palette indices (row 0) */
enum {
    COLOR_BLACK = 0,
    COLOR_WHITE = 1,
    COLOR_YELLOW = 7,
    COLOR_DARK_GRAY = 11,
    COLOR_GRAY = 12,
};

typedef struct {
    const uint8_t *image;   /* 16x16 4bpp, 128 bytes; NULL = unused */
    int x, y;               /* top-left */
    uint8_t z;              /* 0 = hidden, else Z-depth (the game uses 2) */
    bool hflip;
    uint8_t pal;            /* palette offset (row) */
} Sprite;

typedef struct {
    uint8_t bitmap[SCREEN_W * SCREEN_H];    /* layer 0 */
    uint8_t palette[512];                   /* VERA format: GB, R byte pairs */
    uint8_t text_code[TEXT_ROWS][TEXT_COLS];
    uint8_t text_attr[TEXT_ROWS][TEXT_COLS]; /* bg << 4 | fg; bg 0 is transparent */
    uint8_t charset[128 * 8];               /* 1bpp 8x8 */
    Sprite sprites[NSPRITES];
} Vera;

extern Vera vera;

void vera_init(void);

/* Write n bytes of VERA-format palette data starting at byte offset off */
void vera_palette_write(int off, const uint8_t *src, int n);

/* The RGB of palette entry i as 0xFFRRGGBB */
uint32_t vera_rgb(int i);

/* Composite every layer into a SCREEN_W x SCREEN_H frame of 0xAARRGGBB */
void vera_render(uint32_t *frame);

#endif
