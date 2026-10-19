/* The pause menu's Options tab (src/sm_pause_options.c): what one paused
 * frame does to the guest's pause state and pad, and the panel's pixels.
 *
 * The render hashes of the open panel were taken from the original PR #26
 * panel driven through the same frames; the refactor onto the recomp-ui
 * runtime model reproduces them exactly. A change to them is a visible change
 * to the panel. */
#include "sm_pause_options.h"
#include "sm_video.h"
#include "types.h"

#include <assert.h>
#include <stdio.h>

uint8 g_ram[0x20000];
#include "variables.h"

#define game_state_byte g_ram[0x0998]
#define inidisp_byte g_ram[0x0051]

static uint32_t g_frame[684 * 224];

static uint32_t RenderHash(int width) {
  for (size_t i = 0; i < sizeof(g_frame) / sizeof(g_frame[0]); ++i)
    g_frame[i] = 0xFF123456u;
  SmPauseOptions_Render(g_frame, width, width, 224);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < (size_t)width * 224; ++i) { h ^= g_frame[i]; h *= 16777619u; }
  return h;
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
  assert(RenderHash(256) == 0xABBBF0A9u && RenderHash(342) == 0x044A9089u);
  assert(RenderHash(456) == 0xC072CB49u && RenderHash(684) == 0x468098B9u);

  /* HUD anchoring is disabled without widescreen, so Down skips it. */
  Frame(kButton_Down, 0);
  assert(RenderHash(256) == 0x3B3BBF85u && RenderHash(342) == 0xB30D7D8Du);
  Frame(kButton_Down, 0);                   /* aspect ratio */
  Frame(kButton_Right, 0);                  /* Fit wraps to 4:3 */
  assert(g_sm_video.aspect == SM_ASPECT_STOCK);
  assert(RenderHash(256) == 0x93A2A029u && RenderHash(684) == 0xFF6D5E39u);
  Frame(kButton_Right, 0);
  Frame(kButton_Left, 0);
  assert(g_sm_video.aspect == SM_ASPECT_STOCK);
  Frame(kButton_A, 0);                      /* accept steps a choice forward */
  assert(g_sm_video.aspect == SM_ASPECT_16_9);
  assert(RenderHash(256) == 0x887824BFu && RenderHash(342) == 0xEB284A07u);

  /* Turning widescreen on brings HUD anchoring up, from either direction. */
  Frame(kButton_Up, 0);
  Frame(kButton_Up, 0);
  Frame(kButton_Left, 0);
  assert(g_sm_video.enhanced && g_sm_video.hud_anchored);
  Frame(kButton_Down, 0);                   /* now reachable */
  assert(RenderHash(256) == 0xF58E830Fu && RenderHash(342) == 0x69D8329Fu);
  Frame(kButton_A, 0);
  assert(!g_sm_video.hud_anchored && g_sm_video.enhanced);

  /* Back to map: the ROM's Equipment->Map fade-out. The panel stays up,
   * dimmed with the screen, takes no input, and closes at the Load. */
  Frame(kButton_Up, 0);                     /* widescreen */
  Frame(kButton_Up, 0);                     /* wraps to back */
  const uint32_t before_leaving = RenderHash(256);
  Frame(kButton_A, 0);
  assert(SmPauseOptions_IsActive() && SmPauseOptions_Visible());
  assert(menu_index == 5 && pausemenu_button_label_mode == 0);
  assert(RenderHash(256) == before_leaving);          /* full brightness */
  inidisp_byte = 0x07;
  RenderHash(256);
  assert(g_frame[0] == 0xFF04060Bu);        /* COL_BG 0A0E18 at 7/15 */
  inidisp_byte = 0x80;
  RenderHash(256);
  assert(g_frame[0] == 0xFF000000u && g_frame[100 * 256 + 100] == 0xFF000000u);
  Frame(kButton_Down, kButton_R);
  assert(menu_index == 5 && joypad1_newkeys == 0 && joypad1_lastkeys == 0);
  FadeOutDone();
  SmPauseOptions_SyncToGuest();
  assert(!SmPauseOptions_IsActive() && !SmPauseOptions_Visible());
  inidisp_byte = 0x0F;

  /* R on Equipment opens it; R is ignored on Map, as the ROM does. */
  menu_index = 1; pausemenu_button_label_mode = 2;
  Frame(kButton_R, 0);
  assert(SmPauseOptions_IsActive() && menu_index == 0);
  /* Start passes through: the ROM's Map screen unpauses on it. */
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
  assert(RenderHash(256) == 0xABBBF0A9u);

  puts("sm_pause_options: ok");
  return 0;
}
