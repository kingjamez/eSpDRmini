#include "gfx.h"

#include <string.h>

#include "board.h"

static uint16_t *fb;      /* points at screen line 0, which may lie outside the buffer */
static int by0, by1;      /* lines held by the buffer, exclusive end */
static int cx0, cy0, cx1 = LCD_W, cy1 = LCD_H; /* clip, exclusive end */

void gfx_begin(uint16_t *buf, int y0, int rows)
{
    fb = buf - y0 * LCD_W;
    by0 = y0;
    by1 = y0 + rows;
    gfx_noclip();
}

void gfx_clip(int x, int y, int w, int h)
{
    cx0 = x < 0 ? 0 : x;
    cy0 = y < by0 ? by0 : y;
    cx1 = x + w > LCD_W ? LCD_W : x + w;
    cy1 = y + h > by1 ? by1 : y + h;
}

void gfx_noclip(void)
{
    cx0 = 0;
    cy0 = by0;
    cx1 = LCD_W;
    cy1 = by1;
}

uint16_t *gfx_row(int y)
{
    return y >= by0 && y < by1 ? fb + y * LCD_W : NULL;
}

void gfx_clip_rows(int *y0, int *y1)
{
    *y0 = cy0;
    *y1 = cy1;
}

int gfx_rows_visible(int y, int h)
{
    return y < by1 && y + h > by0;
}

static inline int clip_box(int *x, int *y, int *w, int *h)
{
    int x0 = *x < cx0 ? cx0 : *x, y0 = *y < cy0 ? cy0 : *y;
    int x1 = *x + *w > cx1 ? cx1 : *x + *w, y1 = *y + *h > cy1 ? cy1 : *y + *h;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

void gfx_fill(int x, int y, int w, int h, color_t c)
{
    if (!clip_box(&x, &y, &w, &h)) return;
    uint32_t c2 = c | (uint32_t)c << 16;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (y + r) * LCD_W + x;
        int n = w;
        if ((uintptr_t)p & 2) { *p++ = c; n--; }
        uint32_t *q = (uint32_t *)p;
        for (; n >= 8; n -= 8, q += 4) { q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2; }
        for (; n >= 2; n -= 2) *q++ = c2;
        if (n) *(uint16_t *)q = c;
    }
}

void gfx_blend(int x, int y, int w, int h, color_t c, unsigned t)
{
    if (!clip_box(&x, &y, &w, &h)) return;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (y + r) * LCD_W + x;
        for (int i = 0; i < w; i++) p[i] = gfx_mix(p[i], c, t);
    }
}

void gfx_hline(int x, int y, int w, color_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, color_t c) { gfx_fill(x, y, 1, h, c); }

void gfx_rect(int x, int y, int w, int h, color_t c)
{
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

/* Inset of each corner row of a quarter circle of radius r. */
static int corner_inset(int r, int row)
{
    int d = r - row;
    int i = 0;
    while (i < r && (r - i) * (r - i) + d * d > r * r + r / 2) i++;
    return i;
}

void gfx_round_fill(int x, int y, int w, int h, int r, color_t c)
{
    for (int row = 0; row < r; row++) {
        int in = corner_inset(r, row);
        gfx_hline(x + in, y + row, w - 2 * in, c);
        gfx_hline(x + in, y + h - 1 - row, w - 2 * in, c);
    }
    gfx_fill(x, y + r, w, h - 2 * r, c);
}

void gfx_round_rect(int x, int y, int w, int h, int r, color_t c)
{
    gfx_hline(x + r, y, w - 2 * r, c);
    gfx_hline(x + r, y + h - 1, w - 2 * r, c);
    gfx_vline(x, y + r, h - 2 * r, c);
    gfx_vline(x + w - 1, y + r, h - 2 * r, c);
    int end = r;
    for (int row = 0; row < r; row++) {
        int in = corner_inset(r, row);
        int span = end > in + 1 ? end - in : 1;
        gfx_hline(x + in, y + row, span, c);
        gfx_hline(x + w - in - span, y + row, span, c);
        gfx_hline(x + in, y + h - 1 - row, span, c);
        gfx_hline(x + w - in - span, y + h - 1 - row, span, c);
        end = in;
    }
}

void gfx_vdash(int x, int y, int h, int on, int off, color_t c)
{
    for (int i = 0; i < h; i += on + off) gfx_vline(x, y + i, i + on > h ? h - i : on, c);
}

static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp = *p++;
    if (cp >= 0xf0 && p[0] && p[1] && p[2]) {
        cp = (cp & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63);
        p += 3;
    } else if (cp >= 0xe0 && p[0] && p[1]) {
        cp = (cp & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63);
        p += 2;
    } else if (cp >= 0xc0 && p[0]) {
        cp = (cp & 31) << 6 | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return cp;
}

static const glyph_t *find_glyph(const font_t *f, unsigned cp)
{
    if (cp >= 32 && cp < 127 && cp - 32 < f->count && f->glyphs[cp - 32].cp == cp) return &f->glyphs[cp - 32];
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp == '?' ? NULL : find_glyph(f, '?');
}

int gfx_text_width(const font_t *f, const char *s)
{
    int w = 0;
    while (*s) {
        const glyph_t *g = find_glyph(f, utf8_next(&s));
        if (g) w += g->adv;
    }
    return w;
}

static void draw_glyph(const font_t *f, const glyph_t *g, int x, int y, color_t c)
{
    static const uint8_t weight[16] = {0, 2, 4, 6, 9, 11, 13, 15, 17, 19, 21, 23, 26, 28, 30, 32};
    const uint8_t *bits = f->bits + g->off;
    int gx = x + g->xo, gy = y + g->yo;
    for (int r = 0; r < g->h; r++) {
        int py = gy + r;
        if (py < cy0 || py >= cy1) continue;
        uint16_t *row = fb + py * LCD_W;
        for (int col = 0; col < g->w; col++) {
            int px = gx + col;
            unsigned i = r * g->w + col;
            unsigned a = i & 1 ? bits[i >> 1] & 15 : bits[i >> 1] >> 4;
            if (!a || px < cx0 || px >= cx1) continue;
            row[px] = a == 15 ? c : gfx_mix(row[px], c, weight[a]);
        }
    }
}

int gfx_text(const font_t *f, int x, int y, const char *s, color_t c, int align)
{
    int w = gfx_text_width(f, s);
    if (y >= cy1 || y + f->height <= cy0) return w;
    if (align == ALIGN_CENTER) x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;
    while (*s) {
        const glyph_t *g = find_glyph(f, utf8_next(&s));
        if (!g) continue;
        if (g->w) draw_glyph(f, g, x, y, c);
        x += g->adv;
    }
    return w;
}
