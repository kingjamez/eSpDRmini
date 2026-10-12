#include "render.h"

#include "board.h"

void render_plot(int x0, int y0, int w, int h, const color_t *bg, const color_t *under, const int16_t *fill_y,
                 const uint8_t *grid, color_t grid_color)
{
    int r0, r1;
    gfx_clip_rows(&r0, &r1);
    int lo = r0 > y0 ? r0 : y0, hi = r1 < y0 + h ? r1 : y0 + h;
    if (lo >= hi) return;
    uint16_t *base = gfx_row(lo) + x0;
    /* Column by column: two runs of known length, no per-pixel decisions. */
    for (int x = 0; x < w; x++) {
        int split = fill_y[x] < 0 ? hi : fill_y[x] < lo ? lo : fill_y[x] > hi ? hi : fill_y[x];
        uint16_t *p = base + x;
        color_t c = bg[x];
        for (int n = split - lo; n > 0; n--, p += LCD_W) *p = c;
        c = under[x];
        for (int n = hi - split; n > 0; n--, p += LCD_W) *p = c;
    }
    for (int y = lo; y < hi; y++)
        if (grid[y - y0]) {
            uint16_t *p = gfx_row(y) + x0;
            for (int x = 0; x < w; x++) p[x] = grid_color;
        }
}

void trace_spans(const int16_t *ys, int w, int16_t *a, int16_t *b)
{
    int prev = -1;
    for (int x = 0; x < w; x++) {
        int y = ys[x];
        if (y < 0) {
            a[x] = b[x] = prev = -1;
            continue;
        }
        int y0 = prev < 0 ? y : prev, lo = y < y0 ? y : y0, hi = y < y0 ? y0 : y;
        if (hi - lo > 1) {
            if (y < y0) hi = (lo + hi) / 2 + 1;
            else lo = (lo + hi) / 2;
        }
        a[x] = lo;
        b[x] = hi;
        prev = y;
    }
}

void render_spans(int x0, int w, const int16_t *a, const int16_t *b, const color_t *cols, color_t solid, int thick)
{
    int r0, r1;
    gfx_clip_rows(&r0, &r1);
    int grow = thick > 1;
    for (int x = 0; x < w; x++) {
        int g0 = a[x] - grow, g1 = b[x] + 1 + 2 * grow;
        if (a[x] < 0 || g1 <= r0 || g0 >= r1) continue;
        color_t col = cols ? cols[x] : solid;
        int s0 = a[x], s1 = b[x] + 1 + grow;
        int y = g0 < r0 ? r0 : g0, end = g1 > r1 ? r1 : g1;
        uint16_t *p = gfx_row(y) + x0 + x;
        for (; y < end; y++, p += LCD_W) *p = y >= s0 && y < s1 ? col : gfx_mix(*p, col, 12);
    }
}

void render_wf_line(uint16_t *dst, int w, const uint8_t *src, int shift, const color_t *lut, color_t empty)
{
    if (!src || shift <= -w || shift >= w) {
        for (int x = 0; x < w; x++) dst[x] = empty;
        return;
    }
    int a = shift > 0 ? shift : 0, b = shift < 0 ? w + shift : w;
    for (int x = 0; x < a; x++) dst[x] = empty;
    const uint8_t *s = src - shift;
    int x = a;
    for (; x < b && (x & 3); x++) dst[x] = lut[s[x]];
    if (!(((uintptr_t)(s + x) | (uintptr_t)(dst + x)) & 3))
        for (; x + 4 <= b; x += 4) { /* four levels per load, two pixels per store */
            uint32_t v = *(const uint32_t *)(s + x);
            uint32_t *d = (uint32_t *)(dst + x);
            d[0] = lut[v & 255] | (uint32_t)lut[(v >> 8) & 255] << 16;
            d[1] = lut[(v >> 16) & 255] | (uint32_t)lut[v >> 24] << 16;
        }
    for (; x < b; x++) dst[x] = lut[s[x]];
    for (int x = b; x < w; x++) dst[x] = empty;
}
