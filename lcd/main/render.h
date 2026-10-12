/* Per-pixel inner loops of the spectrum and waterfall (kept in IRAM). They
 * draw into the current strip (see gfx_begin). */
#pragma once
#include <stdint.h>

#include "gfx.h"

/* Plot lines y0 .. y0 + h - 1 at columns x0 .. x0 + w - 1: background bg[x]
 * above fill_y[x] and under[x] from it down (no fill where fill_y[x] < 0),
 * then whole grid lines where grid[y - y0] is set. */
void render_plot(int x0, int y0, int w, int h, const color_t *bg, const color_t *under, const int16_t *fill_y,
                 const uint8_t *grid, color_t grid_color);
/* A trace from per-column y (< 0: no data) as vertical spans a[x] .. b[x]
 * that meet their neighbours half way. Computed once per frame. */
void trace_spans(const int16_t *ys, int w, int16_t *a, int16_t *b);
/* Draws those spans; colours per column or solid. Thick traces get a second
 * line and a faint glow around them. */
void render_spans(int x0, int w, const int16_t *a, const int16_t *b, const color_t *cols, color_t solid, int thick);
/* One waterfall line: levels src[] through lut, moved right by shift columns. */
void render_wf_line(uint16_t *dst, int w, const uint8_t *src, int shift, const color_t *lut, color_t empty);
