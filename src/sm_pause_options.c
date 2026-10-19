/* sm_pause_options.c — see sm_pause_options.h for why the third pause tab is
 * a host overlay and not a recompiled screen. */

#include "sm_pause_options.h"

#include "sm_video.h"
#include "snes_overlay_draw.h"

/* Same file src/main.c loads and the Mods page writes. */
static const char *kSmVideoConfig = "sm-video.ini";

enum {
  SM_PO_WIDESCREEN = 0,
  SM_PO_HUD,
  SM_PO_FPS,
  SM_PO_ASPECT,
  SM_PO_BACK,
  SM_PO_COUNT
};

static bool g_active;
static int g_cursor;
static bool g_leave;

/* ── panel colours (ARGB8888) ─────────────────────────────────────────── */
#define COL_BG     0xFF0A0E18u
#define COL_BORDER 0xFF3A4A6Au
#define COL_TITLE  0xFFFFFFFFu
#define COL_TEXT   0xFFC2CCE0u
#define COL_SELBG  0xFF2B4A8Cu
#define COL_SEL    0xFFFFFFFFu
#define COL_DIM    0xFF8A98B4u
#define COL_ACCENT 0xFF6BE06Bu
#define COL_TABON  0xFFFFD24Du

static const char *const kLabels[SM_PO_COUNT] = {
  "WIDESCREEN",
  "HUD ANCHORING",
  "PRESENTATION FPS",
  "ASPECT RATIO",
  "BACK TO MAP",
};

static bool Toggle(bool *value) { *value = !*value; return true; }

/* Whether the row can be interacted with. HUD anchoring only applies when
 * widescreen is on, so it is grayed out / disabled otherwise. */
static bool RowEnabled(int row) {
  switch (row) {
    case SM_PO_HUD: return g_sm_video.enhanced;
    default:        return true;
  }
}

/* Visual indent (in pixels) so HUD anchoring sits suborned under Widescreen. */
static int RowIndent(int row) {
  return row == SM_PO_HUD ? 4 * 8 : 0;
}

static bool CycleAspect(int delta) {
  int a = (int)g_sm_video.aspect + (delta < 0 ? -1 : 1);
  int n = (int)SM_ASPECT_COUNT;
  a = ((a % n) + n) % n;
  g_sm_video.aspect = (SmAspect)a;
  return true;
}

static void SaveSettings(void) { SmVideoSave(&g_sm_video, kSmVideoConfig); }

/* Returns true if the highlighted row changed a setting (so the caller can
 * persist). BACK is an intent, not a setting. */
static bool ApplyRow(int row, int delta) {
  switch (row) {
    case SM_PO_WIDESCREEN:
      // When enabling widescreen, default HUD anchoring to ON (Edges)
      if (delta > 0 && !g_sm_video.enhanced)
        g_sm_video.hud_anchored = true;
      return Toggle(&g_sm_video.enhanced);
    case SM_PO_FPS:        return Toggle(&g_sm_video.fps_enabled);
    case SM_PO_HUD:        return Toggle(&g_sm_video.hud_anchored);
    case SM_PO_ASPECT:     return CycleAspect(delta);
    default:               return false;
  }
}

void SmPauseOptions_SetActive(bool active) {
  if (active && !g_active) {
    g_cursor = 0;
    g_leave = false;
  }
  g_active = active;
}

bool SmPauseOptions_IsActive(void) { return g_active; }

void SmPauseOptions_Move(int delta) {
  if (!g_active || delta == 0) return;
  int c = g_cursor;
  for (int step = 0; step < SM_PO_COUNT; ++step) {
    c += delta;
    if (c < 0) c = SM_PO_COUNT - 1;
    if (c >= SM_PO_COUNT) c = 0;
    if (RowEnabled(c)) {
      g_cursor = c;
      return;
    }
  }
}

void SmPauseOptions_Adjust(int delta) {
  if (!g_active) return;
  if (RowEnabled(g_cursor) && ApplyRow(g_cursor, delta)) SaveSettings();
}

void SmPauseOptions_Activate(void) {
  if (!g_active) return;
  if (g_cursor == SM_PO_BACK)
    g_leave = true;
  else if (RowEnabled(g_cursor) && ApplyRow(g_cursor, 1))
    SaveSettings();
}

bool SmPauseOptions_ConsumeLeaveRequest(void) {
  bool v = g_leave;
  g_leave = false;
  return v;
}

/* ── rendering ────────────────────────────────────────────────────────── */

static const char *RowValue(int row) {
  switch (row) {
    case SM_PO_WIDESCREEN: return g_sm_video.enhanced ? "ON" : "OFF";
    case SM_PO_FPS:        return g_sm_video.fps_enabled ? "ON" : "OFF";
    case SM_PO_HUD:        return g_sm_video.hud_anchored ? "ON" : "OFF";
    case SM_PO_ASPECT:     return SmAspectName(g_sm_video.aspect);
    default:               return "";
  }
}

static void DrawCentered(uint32_t *dst, int stride, int h_max, int width,
                         int y, const char *s, uint32_t col, int scale) {
  int len = 0;
  for (const char *p = s; *p; ++p) ++len;
  int x = (width - len * 8 * scale) / 2;
  if (x < 0) x = 0;
  snes_ovl_draw_text(dst, stride, h_max, x, y, s, col, scale);
}

void SmPauseOptions_Render(uint32_t *dst, int stride, int width, int height) {
  if (!dst || stride <= 0 || height <= 0) return;
  if (width > stride) width = stride;

  /* Opaque backdrop: the guest draws nothing meaningful on this tab (we
   * never load a screen for it), so cover the whole game rect. */
  snes_ovl_fill_rect(dst, stride, height, 0, 0, width, height, COL_BG);
  snes_ovl_stroke_rect(dst, stride, height, 5, 5, width - 10, height - 10, COL_BORDER);

  /* Tab strip: MAP / EQUIP / OPTIONS, with the current tab lit. */
  {
    int y = 12;
    snes_ovl_draw_text(dst, stride, height, 18, y, "MAP", COL_DIM, 1);
    snes_ovl_draw_text(dst, stride, height, 18 + 4 * 8, y, "EQUIP", COL_DIM, 1);
    int ox = 18 + (4 + 6) * 8;
    snes_ovl_fill_rect(dst, stride, height, ox - 3, y - 3, 8 * 8 + 6, 14, COL_SELBG);
    snes_ovl_draw_text(dst, stride, height, ox, y, "OPTIONS", COL_TABON, 1);
  }

  snes_ovl_fill_rect(dst, stride, height, 10, 32, width - 20, 1, COL_BORDER);
  DrawCentered(dst, stride, height, width, 40, "GAME OPTIONS", COL_TITLE, 1);
  snes_ovl_fill_rect(dst, stride, height, 10, 54, width - 20, 1, COL_BORDER);

  /* Rows. */
  {
    int row_y = 66;
    const int row_h = 20;
    for (int i = 0; i < SM_PO_COUNT; ++i, row_y += row_h) {
      bool sel = (i == g_cursor);
      bool enabled = RowEnabled(i);
      int indent  = RowIndent(i);
      uint32_t label_col = sel ? COL_SEL : (enabled ? COL_TEXT : COL_DIM);
      uint32_t value_col = sel ? COL_ACCENT : (enabled ? COL_TEXT : COL_DIM);
      if (sel)
        snes_ovl_fill_rect(dst, stride, height, 14 + indent, row_y - 4,
                          width - 28 - indent, 15, COL_SELBG);
      snes_ovl_draw_text(dst, stride, height, 22 + indent, row_y,
                         kLabels[i], label_col, 1);
      const char *v = RowValue(i);
      if (*v) {
        int len = 0;
        for (const char *p = v; *p; ++p) ++len;
        snes_ovl_draw_text(dst, stride, height,
                           width - 22 - indent - len * 8, row_y, v,
                           value_col, 1);
      }
    }
  }

  /* Hint row. */
  {
    int y = height - 30;
    snes_ovl_fill_rect(dst, stride, height, 10, y - 6, width - 20, 1, COL_BORDER);
    snes_ovl_draw_text(dst, stride, height, 14, y, "A SELECT", COL_ACCENT, 1);
    snes_ovl_draw_text(dst, stride, height, 14 + 10 * 8, y, "B BACK", COL_ACCENT, 1);
    snes_ovl_draw_text(dst, stride, height, 14 + 17 * 8, y, "L/R", COL_ACCENT, 1);
    snes_ovl_draw_text(dst, stride, height, 14 + 21 * 8, y, "TABS", COL_DIM, 1);
  }
}
