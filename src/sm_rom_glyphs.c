/* sm_rom_glyphs.c — M2 ROM glyph renderer (hand-written, NOT src/gen).
 *
 * The glyph bitmaps stay in the cartridge; this file holds only a char→tile
 * metadata index and the 4bpp pixel decode. Every pixel is read through
 * SmRomByte at render time, so no font asset is extracted.
 *
 * The small font the file-select/pause screens draw with is at $B6:$C000
 * (ROM 0x1B4000), 32 bytes per tile: rows 0..7 of planes 0/1 followed by rows
 * 0..7 of planes 2/3. The A-Z subset is byte-identical to the copy the
 * file-select screen loads from ROM 0x70000, so reading it from $B6:$C000
 * keeps the pause data bank as the single source. Verified against captured
 * VRAM:
 *   A-Z  = tiles 0x6a..0x83  (ASCII + 0x29)
 *   0-9  = tiles 0x60..0x69
 *   space is blank; pixel 0 is transparent, 13/14 are the font's shadow/main
 *   tones. A single overlay colour draws both tones (the glyph silhouette).
 *
 * The small font has no '<' or '>' cells, so those two host-only navigation
 * hints are drawn as procedural chevrons in the same colour. */
#include <stdint.h>
#include "sm_rom_glyphs.h"
#include "sm_renderer.h"

enum {
  kFontTileBase = 0xC000,  /* $B6:C000 */
  kFontTileBytes = 32,     /* 4bpp: 8 words planes 0/1 + 8 words planes 2/3 */
  kFontFirstLetter = 0x6a, /* 'A' */
  kFontFirstDigit = 0x60,  /* '0' */
};

/* Char → small-font tile, or -1 for a cell this renderer does not source
 * from the ROM. Lowercase maps to the uppercase glyphs (the font is caps). */
static int CharTile(unsigned ch) {
  if (ch >= 'a' && ch <= 'z') ch -= (unsigned)('a' - 'A');
  if (ch >= 'A' && ch <= 'Z') return kFontFirstLetter + (int)(ch - 'A');
  if (ch >= '0' && ch <= '9') return kFontFirstDigit + (int)(ch - '0');
  return -1;
}

/* Read one 4bpp pixel (0..15) from the cartridge tile. */
static unsigned RomTilePixel(unsigned tile, unsigned x, unsigned y) {
  const unsigned addr = kFontTileBase + tile * kFontTileBytes + y * 2u;
  const unsigned w0 = SmRomByte(0xB6, addr) |
                      ((unsigned)SmRomByte(0xB6, addr + 1u) << 8);
  const unsigned w1 = SmRomByte(0xB6, addr + 16u) |
                      ((unsigned)SmRomByte(0xB6, addr + 17u) << 8);
  const unsigned bit = 7u - x;
  return ((w0 >> bit) & 1u) |
         (((w0 >> (bit + 8u)) & 1u) << 1) |
         (((w1 >> bit) & 1u) << 2) |
         (((w1 >> (bit + 8u)) & 1u) << 3);
}

/* Host-only '<' / '>' chevrons, one bit per pixel, MSB = leftmost. */
static const uint8_t kChevronLeft[8] = {
  0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00,
};
static const uint8_t kChevronRight[8] = {
  0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00,
};

/* Draw an 8x8 one-bit mask with the glyph colour. */
static void DrawMask(uint32_t *dst, int stride, int width, int height,
                     int x, int y, const uint8_t mask[8], uint32_t color,
                     int scale) {
  for (int row = 0; row < 8; ++row) {
    const unsigned bits = mask[row];
    if (!bits) continue;
    for (int col = 0; col < 8; ++col) {
      if (!((bits >> (7 - col)) & 1u)) continue;
      const int bx = x + col * scale;
      const int by = y + row * scale;
      for (int sy = 0; sy < scale; ++sy) {
        const int py = by + sy;
        if (py < 0 || py >= height) continue;
        for (int sx = 0; sx < scale; ++sx) {
          const int px = bx + sx;
          if (px >= 0 && px < width) dst[py * stride + px] = color;
        }
      }
    }
  }
}

void SmRomGlyphDraw(uint32_t *dst, int stride, int width, int height,
                    int x, int y, const char *text, uint32_t color, int scale) {
  if (!dst || !text || !stride || scale < 1) return;
  int cx = x;
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
    if (cx >= width) break;
    const unsigned ch = *p;
    const int tile = CharTile(ch);
    if (tile >= 0) {
      for (int row = 0; row < 8; ++row) {
        const int by = y + row * scale;
        if (by >= height) break;
        for (int col = 0; col < 8; ++col) {
          const int bx = cx + col * scale;
          if (bx >= width) break;
          /* 0 is transparent; 13/14 (shadow/main) are both glyph pixels. */
          if (!RomTilePixel((unsigned)tile, (unsigned)col, (unsigned)row))
            continue;
          for (int sy = 0; sy < scale; ++sy) {
            const int py = by + sy;
            if (py < 0 || py >= height) continue;
            for (int sx = 0; sx < scale; ++sx) {
              const int px = bx + sx;
              if (px >= 0 && px < width) dst[py * stride + px] = color;
            }
          }
        }
      }
    } else if (ch == '<') {
      DrawMask(dst, stride, width, height, cx, y, kChevronLeft, color, scale);
    } else if (ch == '>') {
      DrawMask(dst, stride, width, height, cx, y, kChevronRight, color, scale);
    }
    /* Space and unsupported cells advance blank. */
    cx += 8 * scale;
  }
}
