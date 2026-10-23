/* sm_rom_glyphs.c — M2 ROM glyph and chrome renderer (hand-written, NOT
 * src/gen).
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
 *   space is blank; pixel 0 is transparent and 14 is the main tone. Pixel 13
 *   is the font's bottom-right shadow: drawing it in the main colour makes
 *   the glyph look doubled, so it is drawn in a darker shade of that colour.
 *   The shade is host-derived; no CGRAM/palette data is read or copied.
 *
 * The small font has no '<' or '>' cells, so those two host-only navigation
 * hints are drawn as procedural chevrons in the same colour.
 *
 * The pause-window frame is also sourced in place: the 9-slice tiles
 * 0xbd/0xbe/0xbf/0xcf live in the same BG2 tileset at $B6:8000 and the frame
 * palette is palette 2 at $B6:F000. Nothing is extracted. */
#include <stdint.h>
#include "sm_rom_glyphs.h"
#include "sm_renderer.h"

enum {
  kFontTileBase = 0xC000,   /* $B6:C000 */
  kFrameTileBase = 0x8000,  /* $B6:8000, the pause screen's BG2 tileset */
  kPausePaletteBase = 0xF000, /* $B6:F000, all eight pause palettes */
  kFramePalette = 2,        /* the tilemap's frame entries use palette 2 */
  kFontTileBytes = 32,      /* 4bpp: 8 words planes 0/1 + 8 words planes 2/3 */
  kFontFirstLetter = 0x6a,  /* 'A' */
  kFontFirstDigit = 0x60,   /* '0' */
};

/* Char → small-font tile, or -1 for a cell this renderer does not source
 * from the ROM. Lowercase maps to the uppercase glyphs (the font is caps). */
static int CharTile(unsigned ch) {
  if (ch >= 'a' && ch <= 'z') ch -= (unsigned)('a' - 'A');
  if (ch >= 'A' && ch <= 'Z') return kFontFirstLetter + (int)(ch - 'A');
  if (ch >= '0' && ch <= '9') return kFontFirstDigit + (int)(ch - '0');
  return -1;
}

/* Read one 4bpp pixel (0..15) from a cartridge tile at `base`. */
static unsigned RomTilePixelAt(unsigned base, unsigned tile, unsigned x,
                               unsigned y) {
  const unsigned addr = base + tile * kFontTileBytes + y * 2u;
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

static unsigned RomTilePixel(unsigned tile, unsigned x, unsigned y) {
  return RomTilePixelAt(kFontTileBase, tile, x, y);
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

/* Host-derived shadow shade: 40% of each colour channel. Tone 13 is a
 * bottom-right shadow inside the tile, so painting it in the main colour
 * would fatten the glyph in every direction and read as doubled text. */
static uint32_t GlyphShadow(uint32_t c) {
  const uint32_t r = ((c >> 16) & 0xFFu) * 2u / 5u;
  const uint32_t g = ((c >> 8) & 0xFFu) * 2u / 5u;
  const uint32_t b = (c & 0xFFu) * 2u / 5u;
  return (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

void SmRomGlyphDraw(uint32_t *dst, int stride, int width, int height,
                    int x, int y, const char *text, uint32_t color, int scale) {
  if (!dst || !text || !stride || scale < 1) return;
  const uint32_t shadow = GlyphShadow(color);
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
          /* 0 is transparent; 14 is the main tone, 13 the shadow. */
          const unsigned tone =
              RomTilePixel((unsigned)tile, (unsigned)col, (unsigned)row);
          if (!tone) continue;
          const uint32_t px = tone == 13 ? shadow : color;
          for (int sy = 0; sy < scale; ++sy) {
            const int py = by + sy;
            if (py < 0 || py >= height) continue;
            for (int sx = 0; sx < scale; ++sx) {
              const int px_x = bx + sx;
              if (px_x >= 0 && px_x < width) dst[py * stride + px_x] = px;
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

/* ── pause-window frame ────────────────────────────────────────────────
 *
 * The ROM's pause BG2 tilemap ($B6:E000, loaded to VRAM 0x3800) builds the
 * window from four tiles in the $B6:8000 tileset:
 *
 *   0xbe  horizontal bar (gray/light-gray/white bevel, rows 2..5)
 *   0xcf  vertical bar   (the same bevel, columns 2..5)
 *   0xbf  diagonal corner
 *   0xbd  double-line cap at the ends of the area-label gap
 *
 * The top row is a corner + bars + cap, a 12-tile label gap, cap + bars +
 * corner. The bottom row repeats the bars and corners vertically flipped, and
 * the sides are vertical bars. All frame cells use palette 2, whose entries
 * are read from $B6:F000 at render time (SNES BGR555 -> ARGB8888). */

static unsigned RomWord(unsigned bank, unsigned addr) {
  return (unsigned)SmRomByte(bank, addr) |
         ((unsigned)SmRomByte(bank, addr + 1u) << 8);
}

/* SNES CGRAM colour: 0bbbbbgg gggrrrrr -> opaque ARGB8888. */
static uint32_t SnesToArgb(unsigned c) {
  unsigned r = c & 31u;
  unsigned g = (c >> 5) & 31u;
  unsigned b = (c >> 10) & 31u;
  r = (r << 3) | (r >> 2);
  g = (g << 3) | (g >> 2);
  b = (b << 3) | (b >> 2);
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Draw one 8x8 cartridge tile with optional flips. Tone 0 is transparent;
 * every other tone is looked up in the already-resolved palette. */
static void DrawRomTile(uint32_t *dst, int stride, int width, int height,
                        int x, int y, unsigned tile, const uint32_t pal[16],
                        int hflip, int vflip) {
  for (int row = 0; row < 8; ++row) {
    const int py = y + row;
    if (py < 0 || py >= height) continue;
    const int sy = vflip ? 7 - row : row;
    for (int col = 0; col < 8; ++col) {
      const int px = x + col;
      if (px < 0 || px >= width) continue;
      const int sx = hflip ? 7 - col : col;
      const unsigned tone =
          RomTilePixelAt(kFrameTileBase, tile, (unsigned)sx, (unsigned)sy);
      if (!tone) continue;
      dst[py * stride + px] = pal[tone];
    }
  }
}

void SmRomPauseFrameDraw(uint32_t *dst, int stride, int width, int height,
                         int x, int y, int w, int h,
                         int gap_col, int gap_tiles) {
  if (!dst || stride <= 0 || w < 24 || h < 24) return;
  const int cols = w / 8;
  const int rows = h / 8;
  if (cols < 3 || rows < 3) return;
  if (gap_tiles < 0) gap_tiles = 0;

  uint32_t pal[16];
  const unsigned base =
      kPausePaletteBase + (unsigned)kFramePalette * 16u * 2u;
  for (unsigned i = 0; i < 16; ++i)
    pal[i] = SnesToArgb(RomWord(0xB6, base + i * 2u));

  /* Top edge: corners, bars, an optional label gap capped with 0xbd. */
  for (int c = 0; c < cols; ++c) {
    unsigned tile;
    int hflip = 0;
    if (c == 0) {
      tile = 0xbf;
      hflip = 1;
    } else if (c == cols - 1) {
      tile = 0xbf;
    } else if (gap_tiles && c >= gap_col && c < gap_col + gap_tiles) {
      continue;  /* label sits here */
    } else if (gap_tiles && c == gap_col - 1) {
      tile = 0xbd;
      hflip = 1;
    } else if (gap_tiles && c == gap_col + gap_tiles) {
      tile = 0xbd;
    } else {
      tile = 0xbe;
    }
    DrawRomTile(dst, stride, width, height, x + c * 8, y, tile, pal, hflip, 0);
  }

  /* Bottom edge: continuous bars, corners flipped vertically. */
  for (int c = 0; c < cols; ++c) {
    unsigned tile = (c == 0 || c == cols - 1) ? 0xbf : 0xbe;
    int hflip = (c == 0);
    DrawRomTile(dst, stride, width, height, x + c * 8, y + (rows - 1) * 8,
                tile, pal, hflip, 1);
  }

  /* Sides: 0xcf is asymmetric, so the left side is mirrored. */
  for (int r = 1; r < rows - 1; ++r) {
    DrawRomTile(dst, stride, width, height, x, y + r * 8, 0xcf, pal, 1, 0);
    DrawRomTile(dst, stride, width, height, x + (cols - 1) * 8, y + r * 8,
                0xcf, pal, 0, 0);
  }
}
