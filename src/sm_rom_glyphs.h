/* sm_rom_glyphs.h — M2 ROM glyph and chrome renderer (hand-written, NOT
 * src/gen). The small-font tiles stay in the cartridge at $B6:$C000 and the
 * pause-window tiles/palette at $B6:$8000 / $B6:$F000; this module reads them
 * through SmRomByte at render time. No glyph, tile or palette bitmap is
 * extracted. */
#pragma once
#include <stdint.h>

/* Draw an 8x8-per-cell string using the ROM's 4bpp small font (A-Z, 0-9).
 * '<' and '>' are host-drawn chevrons (the font has no such tiles); space and
 * unsupported characters advance blank. Pixel value 0 is transparent, 14 is
 * the main tone and 13 is the font's shadow, drawn in a darker shade of
 * `color` so the glyph does not read as doubled.
 * dst/stride/width/height: output framebuffer (ARGB8888) and dimensions.
 * x,y: top-left destination position. text: null-terminated ASCII string.
 * color: ARGB color for opaque glyph pixels. scale: pixel multiplier
 * (1 = 8×8 native, 2 = 16×16, etc.). */
void SmRomGlyphDraw(uint32_t *dst, int stride, int width, int height,
                    int x, int y, const char *text, uint32_t color, int scale);

/* Draw the ROM pause-window frame as a 9-slice of the cartridge's BG2 tiles
 * from $B6:8000 (0xbd/0xbe/0xbf/0xcf), with palette 2 read from $B6:F000 at
 * render time. No tile or palette data is copied out of the ROM.
 * dst/stride/width/height: output framebuffer (ARGB8888) and dimensions.
 * x,y,w,h: frame rectangle; w and h should be multiples of 8 (native size is
 * 256x160 at y=40 in the pause screen).
 * gap_col/gap_tiles: omit that run of top-edge cells for a label; the gap is
 * capped with tile 0xbd at each end. Pass gap_tiles = 0 for an unbroken edge. */
void SmRomPauseFrameDraw(uint32_t *dst, int stride, int width, int height,
                         int x, int y, int w, int h,
                         int gap_col, int gap_tiles);
