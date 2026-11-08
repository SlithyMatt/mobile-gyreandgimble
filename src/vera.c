#include "vera.h"

#include <string.h>

#include "font_petscii.h"

Vera vera;

/* The X16's default palette, first row: the text colors (12-bit RGB) */
static const uint16_t default_row0[16] = {
    0x000, 0xfff, 0x800, 0xafe, 0xc4c, 0x0c5, 0x00a, 0xee7,
    0xd85, 0x640, 0xf77, 0x333, 0x777, 0xaf6, 0x08f, 0xbbb,
};

void vera_init(void)
{
    memset(&vera, 0, sizeof vera);
    for (int i = 0; i < 16; i++) {
        vera.palette[i * 2] = default_row0[i] & 0xff;      /* GB */
        vera.palette[i * 2 + 1] = default_row0[i] >> 8;    /* R */
    }
    memcpy(vera.charset, font_petscii_lower, sizeof vera.charset);
    memset(vera.text_code, 0x20, sizeof vera.text_code);
    memset(vera.text_attr, COLOR_WHITE, sizeof vera.text_attr);
}

void vera_palette_write(int off, const uint8_t *src, int n)
{
    if (off < 0 || off + n > (int)sizeof vera.palette)
        return;
    memcpy(vera.palette + off, src, n);
}

uint32_t vera_rgb(int i)
{
    uint8_t gb = vera.palette[i * 2], r = vera.palette[i * 2 + 1] & 0x0f;
    uint32_t R = r * 17, G = (gb >> 4) * 17, B = (gb & 0x0f) * 17;
    return 0xff000000u | R << 16 | G << 8 | B;
}

void vera_render(uint32_t *frame)
{
    uint32_t rgb[256];
    for (int i = 0; i < 256; i++)
        rgb[i] = vera_rgb(i);

    /* Layer 0: the bitmap (index 0 shows the black border color) */
    for (int k = 0; k < SCREEN_W * SCREEN_H; k++)
        frame[k] = rgb[vera.bitmap[k]];

    /* Sprites at Z-depth 2, over layer 0; a lower index draws on top */
    for (int s = NSPRITES - 1; s >= 0; s--) {
        const Sprite *sp = &vera.sprites[s];
        if (!sp->image || !sp->z)
            continue;
        for (int y = 0; y < 16; y++) {
            int py = sp->y + y;
            if (py < 0 || py >= SCREEN_H)
                continue;
            for (int x = 0; x < 16; x++) {
                int px = sp->x + x;
                if (px < 0 || px >= SCREEN_W)
                    continue;
                int sx = sp->hflip ? 15 - x : x;
                uint8_t b = sp->image[y * 8 + sx / 2];
                uint8_t c = sx & 1 ? b & 0x0f : b >> 4;
                if (c)
                    frame[py * SCREEN_W + px] = rgb[sp->pal << 4 | c];
            }
        }
    }

    /* Layer 1: text, over everything; background color 0 is transparent */
    for (int row = 0; row < TEXT_ROWS; row++) {
        for (int col = 0; col < TEXT_COLS; col++) {
            uint8_t code = vera.text_code[row][col], attr = vera.text_attr[row][col];
            uint8_t fg = attr & 0x0f, bg = attr >> 4;
            const uint8_t *glyph = &vera.charset[(code & 0x7f) * 8];
            bool reverse = code & 0x80;
            for (int y = 0; y < 8; y++) {
                uint8_t bits = glyph[y] ^ (reverse ? 0xff : 0);
                uint32_t *dst = &frame[(row * 8 + y) * SCREEN_W + col * 8];
                for (int x = 0; x < 8; x++) {
                    if (bits & (0x80 >> x))
                        dst[x] = rgb[fg];
                    else if (bg)
                        dst[x] = rgb[bg];
                }
            }
        }
    }
}
