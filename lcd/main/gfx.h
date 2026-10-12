/* Minimal RGB565 drawing into a frame buffer, with anti-aliased text. */
#pragma once
#include <stdint.h>

typedef uint16_t color_t;
#define RGB(hex) ((color_t)((((hex) >> 8) & 0xf800) | (((hex) >> 5) & 0x07e0) | (((hex) >> 3) & 0x001f)))

typedef struct {
    uint16_t cp;      /* code point */
    uint8_t w, h;     /* bitmap size */
    int8_t xo, yo;    /* bitmap offset from the pen position and the line top */
    uint8_t adv;
    uint32_t off;     /* into the font's 4-bit bitmap data */
} glyph_t;

typedef struct {
    const glyph_t *glyphs;
    uint16_t count;
    uint8_t ascent, height;
    const uint8_t *bits;
} font_t;

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/* Drawing goes to a buffer holding screen lines y0 .. y0 + rows - 1, full
 * width; everything is clipped to it (and to an optional clip box). */
void gfx_begin(uint16_t *buf, int y0, int rows);
void gfx_clip(int x, int y, int w, int h);
void gfx_noclip(void);
/* Pixels of screen line y, or NULL when the line is outside the buffer. */
uint16_t *gfx_row(int y);
/* Lines the current clip allows, y0 inclusive to y1 exclusive. */
void gfx_clip_rows(int *y0, int *y1);
/* True if any of screen lines y .. y + h - 1 are in the buffer. */
int gfx_rows_visible(int y, int h);

/* Blends all three fields at once: each sits in a gap wide enough for the
 * multiply (the 0x07e0f81f layout). t = 0..32 is the weight of b. */
static inline color_t gfx_mix(color_t a, color_t b, unsigned t)
{
    uint32_t x = (a | (uint32_t)a << 16) & 0x07e0f81fu;
    uint32_t y = (b | (uint32_t)b << 16) & 0x07e0f81fu;
    uint32_t r = (x + (((y - x) * t) >> 5)) & 0x07e0f81fu;
    return (color_t)(r | r >> 16);
}
void gfx_fill(int x, int y, int w, int h, color_t c);
void gfx_blend(int x, int y, int w, int h, color_t c, unsigned t);
void gfx_hline(int x, int y, int w, color_t c);
void gfx_vline(int x, int y, int h, color_t c);
void gfx_rect(int x, int y, int w, int h, color_t c);
void gfx_round_fill(int x, int y, int w, int h, int r, color_t c);
void gfx_round_rect(int x, int y, int w, int h, int r, color_t c);
void gfx_vdash(int x, int y, int h, int on, int off, color_t c);
/* Text: y is the top of the line box. Returns the advance width. */
int gfx_text(const font_t *f, int x, int y, const char *s, color_t c, int align);
int gfx_text_width(const font_t *f, const char *s);
