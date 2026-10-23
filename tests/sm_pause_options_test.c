/* The pause menu's Options tab (src/sm_pause_options.c): what one paused
 * frame does to the guest's pause state and pad, and the panel's pixels.
 *
 * The render hashes are a golden picture of the open panel at its eleven-row
 * layout (five SM video settings, five framework display settings, a
 * graceful Exit, and Back) with the navigation the test has driven. A
 * change to them is a visible change to the panel.
 *
 * The framework's live-settings API (host_api.h) is faked here: the fakes
 * apply the same clamps the host's setters do and mark g_config_dirty, as
 * the host does before its shutdown config.ini write. No SDL runs.
 */
#include "sm_pause_options.h"
#include "sm_video.h"
#include "types.h"
#include "host_api.h"

#include <assert.h>
#include <stdio.h>

uint8 g_ram[0x20000];
#include "variables.h"

#define game_state_byte g_ram[0x0998]
#define inidisp_byte g_ram[0x0051]

/* host_api.h fakes. The host applies the same clamps (runner/src/desktop/
 * host_main.c); the defaults match the shipped config.ini: vsync on, window
 * scale 3, everything else off. */
bool g_config_dirty;
bool f_host_live = true;
bool snesrecomp_host_frame_is_live(void) { return f_host_live; }
bool f_exit_requested;
int f_fullscreen;        /* 0 = windowed, 1 = borderless */
int f_vsync = 1;         /* 0 = off, 1 = on, 2 = adaptive */
int f_frame_blend;
int f_run_ahead;
int f_window_scale = 3;

void snesrecomp_set_fullscreen(int mode) {
  if (mode < 0) mode = 0;
  if (mode > 1) mode = 1;
  f_fullscreen = mode;
  g_config_dirty = true;
}
int snesrecomp_get_fullscreen(void) { return f_fullscreen; }

void snesrecomp_set_vsync(int mode) {
  if (mode < 0) mode = 0;
  if (mode > 2) mode = 2;
  f_vsync = mode;
  g_config_dirty = true;
}
int snesrecomp_get_vsync(void) { return f_vsync; }

void snesrecomp_set_frame_blend(int enabled) {
  f_frame_blend = enabled != 0;
  g_config_dirty = true;
}
int snesrecomp_get_frame_blend(void) { return f_frame_blend; }

void snesrecomp_set_run_ahead(int frames) {
  if (frames < 0) frames = 0;
  f_run_ahead = frames;
  g_config_dirty = true;
}
int snesrecomp_get_run_ahead(void) { return f_run_ahead; }

void snesrecomp_set_window_scale(int scale) {
  if (scale < 1) scale = 1;
  if (scale > 10) scale = 10;
  f_window_scale = scale;
  g_config_dirty = true;
}
int snesrecomp_get_window_scale(void) { return f_window_scale; }

/* The host ends the run after the requesting frame; the fake only latches,
 * so the test can keep driving the model afterwards. */
void snesrecomp_request_exit(void) { if (f_host_live) f_exit_requested = true; }

static uint32_t g_frame[684 * 224];


/* Test stubs for the ROM glyph renderer. The glyph bitmaps live in the
 * cartridge; the test supplies a synthetic font region so the render is
 * deterministic without the real ROM. No ROM asset is copied here: the
 * pattern below is generated, not extracted. */
static uint8 g_fake_rom[0x10000];   /* stands in for $B6:8000-$B6:FFFF */

static void init_fake_rom(void) {
  memset(g_fake_rom, 0, sizeof(g_fake_rom));
  /* Every font tile gets a distinct 4bpp pattern at $B6:C000 + tile*32
   * (offset 0x4000 in this window), so a wrong char->tile index or a
   * plane/row mix-up changes the pixels and the golden hash catches it.
   * Clearing the top bit of each plane byte leaves the leftmost pixel
   * transparent in every row, exercising the decode's zero handling. */
  for (unsigned tile = 0x60; tile <= 0x8f; ++tile) {
    unsigned base = 0x4000u + tile * 32u;
    for (unsigned y = 0; y < 8; ++y) {
      uint32_t s = tile * 2654435761u + y * 97u;
      uint16_t w0 = (uint16_t)((s >> 3) & 0x7f7fu);
      uint16_t w1 = (uint16_t)(((s >> 19) ^ (s >> 7)) & 0x7f7fu);
      g_fake_rom[base + y * 2] = (uint8_t)(w0 & 0xFF);
      g_fake_rom[base + y * 2 + 1] = (uint8_t)(w0 >> 8);
      g_fake_rom[base + 16 + y * 2] = (uint8_t)(w1 & 0xFF);
      g_fake_rom[base + 16 + y * 2 + 1] = (uint8_t)(w1 >> 8);
    }
  }

  /* The window frame reads tiles 0xbd/0xbe/0xbf/0xcf from $B6:8000 (offset
   * 0 here) and palette 2 from $B6:F000 (offset 0x7000). Give both synthetic
   * contents so the frame's tile decode, flips and palette lookup are covered
   * by the golden hash. Generated, not extracted. */
  {
    static const unsigned frame_tiles[] = {0xbd, 0xbe, 0xbf, 0xcf};
    for (size_t t = 0; t < sizeof(frame_tiles) / sizeof(frame_tiles[0]); ++t) {
      const unsigned tile = frame_tiles[t];
      const unsigned base = tile * 32u;
      for (unsigned y = 0; y < 8; ++y) {
        uint32_t s = tile * 2246822519u + y * 59u;
        uint16_t w0 = (uint16_t)((s >> 5) & 0x7f7fu);
        uint16_t w1 = (uint16_t)(((s >> 21) ^ (s >> 3)) & 0x7f7fu);
        g_fake_rom[base + y * 2] = (uint8_t)(w0 & 0xFF);
        g_fake_rom[base + y * 2 + 1] = (uint8_t)(w0 >> 8);
        g_fake_rom[base + 16 + y * 2] = (uint8_t)(w1 & 0xFF);
        g_fake_rom[base + 16 + y * 2 + 1] = (uint8_t)(w1 >> 8);
      }
    }
    /* Palette 2, one distinct BGR555 entry per index. */
    const unsigned pal = 0x7000u + 2u * 16u * 2u;
    for (unsigned i = 0; i < 16; ++i) {
      const uint16_t c = (uint16_t)(0x0421u + i * 0x0421u);
      g_fake_rom[pal + i * 2] = (uint8_t)(c & 0xFF);
      g_fake_rom[pal + i * 2 + 1] = (uint8_t)(c >> 8);
    }
  }
}

const uint8_t *SmRomPtr(unsigned bank, unsigned addr, size_t size) {
  if (bank != 0xB6 || addr < 0x8000) return NULL;
  size_t off = addr - 0x8000;
  if (off + size > sizeof(g_fake_rom)) return NULL;
  return &g_fake_rom[off];
}

uint8_t SmRomByte(unsigned bank, unsigned addr) {
  const uint8_t *p = SmRomPtr(bank, addr, 1);
  return p ? *p : 0;
}

static uint32_t RenderHash(int width) {
  for (size_t i = 0; i < sizeof(g_frame) / sizeof(g_frame[0]); ++i)
    g_frame[i] = 0xFF123456u;
  SmPauseOptions_Render(g_frame, width, width, 224);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < (size_t)width * 224; ++i) { h ^= g_frame[i]; h *= 16777619u; }
  return h;
}

/* A golden-pixel check. In a calibration build (PAUSE_OPTIONS_TEST_CALIBRATE,
 * compiled by hand after a layout or row change) it prints the actual hash
 * instead of asserting, so the constants below can be regenerated. */
#ifndef PAUSE_OPTIONS_TEST_CALIBRATE
#define PAUSE_OPTIONS_TEST_CALIBRATE 0
#endif

static size_t g_hash_seq;

static void HashCheck(int width, uint32_t expected) {
  const uint32_t actual = RenderHash(width);
#if PAUSE_OPTIONS_TEST_CALIBRATE
  printf("hash %zu width=%d actual=0x%08X expected=0x%08X\n",
         ++g_hash_seq, width, actual, expected);
#else
  assert(actual == expected);
#endif
}

/* One paused frame with `pressed` newly down (and `held` also down). */
static void Frame(uint16 pressed, uint16 held) {
  joypad1_newkeys = pressed;
  joypad1_lastkeys = (uint16)(pressed | held);
  newly_held_down_timed_held_input = pressed;
  SmPauseOptions_GuestTick();
}

static void OnMap(void) { menu_index = 0; pausemenu_button_label_mode = 0; }

/* The ROM finishing a fade-out: it moves on to the Load state. */
static void FadeOutDone(void) { menu_index = (uint16)(menu_index + 1); }

int main(void) {
  init_fake_rom();
  SmVideoDefaults(&g_sm_video);
  g_sm_video.hud_anchored = false;
  assert(!g_sm_video.enhanced && !g_sm_video.fps_enabled);
  game_state_byte = 0x0F;
  inidisp_byte = 0x0F;

  /* L on Map opens the panel. While it is up it owns the pad: held buttons
   * are swallowed too, so the hidden Map screen sees none of them. */
  OnMap();
  Frame(kButton_L, kButton_Y | kButton_Down);
  assert(SmPauseOptions_IsActive() && SmPauseOptions_Visible() && menu_index == 0);
  assert(joypad1_newkeys == 0 && newly_held_down_timed_held_input == 0);
  assert(joypad1_lastkeys == 0);
  Frame(0, kButton_Down);
  assert(joypad1_lastkeys == 0);
  /* Opened on Widescreen at the defaults: sm-video defaults, and the faked
   * framework defaults (windowed, vsync on, no blend, no run-ahead, scale 3). */
  HashCheck(256, 0x4B81A497u);
  HashCheck(342, 0x7794CC3Fu);
  HashCheck(456, 0xE1D685B7u);
  HashCheck(684, 0x3403F8C7u);
  {
    /* The window frame comes from the synthetic ROM tiles, not a host
     * rectangle: the top-left corner (4,42) and the top bar (12,42) are
     * opaque frame pixels rather than the panel background. */
    (void)RenderHash(256);
    assert(g_frame[42 * 256 + 4] != 0xFF0A0E18u);
    assert(g_frame[42 * 256 + 12] != 0xFF0A0E18u);
  }

  /* HUD anchoring is disabled without widescreen, so Down skips it. */
  Frame(kButton_Down, 0);   /* Presentation FPS */
  HashCheck(256, 0x919B011Fu);
  HashCheck(342, 0x143D4DDFu);
  Frame(kButton_Down, 0);   /* Aspect ratio */
  Frame(kButton_Right, 0);  /* Fit wraps to 4:3 */
  assert(g_sm_video.aspect == SM_ASPECT_STOCK);
  HashCheck(256, 0x00BBAD3Fu);
  HashCheck(684, 0x73F1B9DFu);
  Frame(kButton_Right, 0);
  Frame(kButton_Left, 0);
  assert(g_sm_video.aspect == SM_ASPECT_STOCK);
  Frame(kButton_A, 0);      /* accept steps a choice forward */
  assert(g_sm_video.aspect == SM_ASPECT_16_9);
  HashCheck(256, 0x826303BDu);
  HashCheck(342, 0x162A6371u);

  /* Turning widescreen on brings HUD anchoring up, from either direction. */
  Frame(kButton_Up, 0);     /* Presentation FPS */
  Frame(kButton_Up, 0);     /* skips the disabled HUD row to Widescreen */
  Frame(kButton_Left, 0);
  assert(g_sm_video.enhanced && g_sm_video.hud_anchored);
  Frame(kButton_Down, 0);   /* HUD anchoring, now reachable */
  HashCheck(256, 0x6F0795C1u);
  HashCheck(342, 0xCB33DFE5u);
  Frame(kButton_A, 0);
  assert(!g_sm_video.hud_anchored && g_sm_video.enhanced);

  /* Down leaves the SM rows for the framework's: Fullscreen, Windowed. */
  Frame(kButton_Down, 0);   /* Presentation FPS */
  Frame(kButton_Down, 0);   /* Aspect ratio */
  Frame(kButton_Down, 0);   /* Fullscreen */
  HashCheck(256, 0x62DE0B4Fu);
  HashCheck(684, 0xFCF0669Fu);
  Frame(kButton_Right, 0);  /* Borderless */
  assert(f_fullscreen == 1 && g_config_dirty);
  HashCheck(256, 0xFBCF02DCu);

  /* VSync is a three-way choice that wraps at both ends. */
  Frame(kButton_Down, 0);   /* VSync */
  Frame(kButton_Right, 0);
  assert(f_vsync == 2 && g_config_dirty);   /* Adaptive */
  Frame(kButton_Right, 0);
  assert(f_vsync == 0);                     /* Off */
  Frame(kButton_Right, 0);
  assert(f_vsync == 1);                     /* back to On */
  HashCheck(256, 0xB2E1F502u);

  /* Frame blend toggles on a fresh press, either direction. */
  Frame(kButton_Down, 0);   /* Frame blend */
  Frame(kButton_A, 0);
  assert(f_frame_blend == 1 && g_config_dirty);
  Frame(kButton_A, 0);
  assert(f_frame_blend == 0);
  HashCheck(256, 0x35A7FFA0u);

  /* Run ahead counts 0..4 and stops at the ends. */
  Frame(kButton_Down, 0);   /* Run ahead */
  Frame(kButton_Right, 0);
  Frame(kButton_Right, 0);
  Frame(kButton_Right, 0);
  Frame(kButton_Right, 0);
  assert(f_run_ahead == 4);
  Frame(kButton_Right, 0);
  assert(f_run_ahead == 4);
  HashCheck(256, 0x96908962u);
  Frame(kButton_Left, 0);
  assert(f_run_ahead == 3);
  Frame(kButton_Left, 0);
  Frame(kButton_Left, 0);
  Frame(kButton_Left, 0);
  Frame(kButton_Left, 0);
  assert(f_run_ahead == 0);
  Frame(kButton_Left, 0);
  assert(f_run_ahead == 0);

  /* Still borderless, so Window scale is disabled: navigation skips it and
   * lands on Exit. */
  Frame(kButton_Down, 0);   /* Run ahead -> Exit */
  HashCheck(256, 0xC4130502u);
  HashCheck(684, 0xB71BE37Au);
  Frame(kButton_A, 0);      /* graceful exit */
  assert(f_exit_requested);
  HashCheck(256, 0xC4130502u);       /* the action changes nothing visible */

  /* Down to Back, up again to Fullscreen (over the disabled scale row). */
  Frame(kButton_Down, 0);   /* Back to map */
  HashCheck(256, 0x3C27B33Eu);
  Frame(kButton_Up, 0);
  Frame(kButton_Up, 0);
  Frame(kButton_Up, 0);
  Frame(kButton_Up, 0);
  Frame(kButton_Up, 0);     /* Fullscreen */
  Frame(kButton_Left, 0);   /* Windowed */
  assert(f_fullscreen == 0 && g_config_dirty);
  HashCheck(256, 0x62DE0B4Fu);

  /* Window scale, enabled again, counts 1..10. */
  Frame(kButton_Down, 0);   /* VSync */
  Frame(kButton_Down, 0);   /* Frame blend */
  Frame(kButton_Down, 0);   /* Run ahead */
  Frame(kButton_Down, 0);   /* Window scale */
  Frame(kButton_Right, 0);
  Frame(kButton_Right, 0);
  assert(f_window_scale == 5);
  Frame(kButton_Left, 0);
  assert(f_window_scale == 4);
  HashCheck(256, 0x83C0E90Au);
  HashCheck(684, 0x8C16E0E2u);

  /* Back: the ROM's Equipment->Map fade-out. The panel stays up,
   * dimmed with the screen, takes no input, and closes at the Load. */
  Frame(kButton_Down, 0);   /* Exit */
  Frame(kButton_Down, 0);   /* Back to map */
  const uint32_t before_leaving = RenderHash(256);
  Frame(kButton_A, 0);
  assert(SmPauseOptions_IsActive() && SmPauseOptions_Visible());
  assert(menu_index == 5 && pausemenu_button_label_mode == 0);
  assert(RenderHash(256) == before_leaving);          /* full brightness */
  inidisp_byte = 0x07;
  RenderHash(256);
#if !PAUSE_OPTIONS_TEST_CALIBRATE
  assert(g_frame[0] == 0xFF04060Bu);        /* COL_BG 0A0E18 at 7/15 */
#endif
  inidisp_byte = 0x80;
  RenderHash(256);
#if !PAUSE_OPTIONS_TEST_CALIBRATE
  assert(g_frame[0] == 0xFF000000u && g_frame[100 * 256 + 100] == 0xFF000000u);
#endif
  Frame(kButton_Down, kButton_R);
  assert(menu_index == 5 && joypad1_newkeys == 0 && joypad1_lastkeys == 0);
  FadeOutDone();
  SmPauseOptions_SyncToGuest();
  assert(!SmPauseOptions_IsActive() && !SmPauseOptions_Visible());
  inidisp_byte = 0x0F;

  /* R on Equipment opens it over the guest's real screen (no-pin model),
   * leaving menu_index on Equipment; R is ignored on Map, as the ROM does. */
  menu_index = 1; pausemenu_button_label_mode = 2;
  Frame(kButton_R, 0);
  assert(SmPauseOptions_IsActive() && menu_index == 1);
  /* Start passes through: the pause screen under the panel unpauses on it. */
  Frame(kButton_Start, kButton_A);
  assert(joypad1_newkeys == kButton_Start && newly_held_down_timed_held_input == kButton_Start);
  assert(joypad1_lastkeys == kButton_Start);
  /* L: the ROM's Map->Equipment fade-out. */
  Frame(kButton_L, 0);
  assert(SmPauseOptions_IsActive() && menu_index == 2 && pausemenu_button_label_mode == 2);
  FadeOutDone();
  Frame(0, 0);
  assert(!SmPauseOptions_IsActive() && menu_index == 3);

  OnMap();
  Frame(kButton_R, 0);
  assert(!SmPauseOptions_IsActive() && joypad1_newkeys == kButton_R);
  Frame(kButton_L, 0);
  Frame(kButton_R, 0);
  assert(SmPauseOptions_IsActive() && menu_index == 5 && joypad1_newkeys == 0);
  FadeOutDone();
  SmPauseOptions_SyncToGuest();
  OnMap();
  Frame(kButton_L, 0);
  Frame(kButton_B, 0);
  assert(SmPauseOptions_IsActive() && menu_index == 5 && joypad1_newkeys == 0);
  FadeOutDone();
  SmPauseOptions_SyncToGuest();
  assert(!SmPauseOptions_IsActive());

  /* Start: the panel stays up through the unpause fade-out ($10), then goes. */
  OnMap();
  Frame(kButton_L, 0);
  game_state_byte = 0x10;
  SmPauseOptions_SyncToGuest();
  assert(SmPauseOptions_Visible());
  game_state_byte = 0x11;
  SmPauseOptions_SyncToGuest();
  assert(!SmPauseOptions_IsActive() && !SmPauseOptions_Visible());
  game_state_byte = 0x0F;

  /* Snapshots restore the flag; a restored panel opens on its first row. */
  OnMap();
  Frame(kButton_L, 0);
  Frame(kButton_Down, 0);
  SmPauseOptions_SetActive(false);
  SmPauseOptions_SetActive(true);
  g_sm_video.enhanced = false; g_sm_video.hud_anchored = false;
  g_sm_video.fps_enabled = false; g_sm_video.aspect = SM_ASPECT_FIT;
  f_fullscreen = 0; f_vsync = 1; f_frame_blend = 0; f_run_ahead = 0;
  f_window_scale = 3; f_exit_requested = false; g_config_dirty = false;
  /* The opening picture again. */
  HashCheck(256, 0x4B81A497u);

  puts("sm_pause_options: ok");
  return 0;
}
