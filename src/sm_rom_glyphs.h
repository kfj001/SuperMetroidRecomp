/* sm_rom_glyphs.h — M2 ROM glyph renderer (hand-written, NOT src/gen).
 * The small-font tiles stay in the cartridge at $B6:$C000; this module reads
 * them through SmRomByte at render time. No glyph bitmap is extracted. */
#pragma once
#include <stdint.h>

/* Draw an 8x8-per-cell string using the ROM's 4bpp small font (A-Z, 0-9).
 * '<' and '>' are host-drawn chevrons (the font has no such tiles); space and
 * unsupported characters advance blank. Pixel value 0 is transparent and both
 * font tones (13/14) are drawn in `color`.
 * dst/stride/width/height: output framebuffer (ARGB8888) and dimensions.
 * x,y: top-left destination position. text: null-terminated ASCII string.
 * color: ARGB color for opaque glyph pixels. scale: pixel multiplier
 * (1 = 8×8 native, 2 = 16×16, etc.). */
void SmRomGlyphDraw(uint32_t *dst, int stride, int width, int height,
                    int x, int y, const char *text, uint32_t color, int scale);
