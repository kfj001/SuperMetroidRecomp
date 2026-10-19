#pragma once

/*
 * Game Options — the third pause-menu tab.
 *
 * Super Metroid's pause menu has room for exactly two tabs (Map and
 * Equipment); the ROM's L/R handler hard-codes `menu_index = 2|5` and the
 * button-label tilemap only has two highlighted states. Its menus are also
 * baked: text on the pause screen is pre-composed tilemap data DMA'd out of
 * the ROM, with no string compositor to hang a new screen off
 * (EquipmentScreenTransferBG1Tilemap -> ram3800 is the only writer, and it
 * copies fixed blobs).
 *
 * So the third tab lives where the framework already knows how to draw text:
 * the host's shared overlay primitives (snes_overlay_draw.h, the same 8x8
 * font the save-state browser uses). The guest side only owns navigation —
 * HleSmHandlePauseScreenLrInput cycles Map/Equipment/Options and parks on
 * menu_index == SM_MENU_INDEX_OPTIONS; this module owns what that screen
 * shows and does.
 *
 * The rows edit g_sm_video, the same struct src/main.c loads, the Mods page
 * writes and SmVideoSave persists. There is deliberately no "quit" row: the
 * framework exposes no guest-accessible way to leave the session (the old
 * scaffold's kGameState_ExitToLauncher = 0xFF was written but never read by
 * anything), so a quit row would have been an inert promise.
 *
 * The guest HLEs (src/gen_stubs.c) call the input entry points; src/main.c's
 * draw_frame hook composites Render() over the frame whenever IsActive().
 */

#include <stdbool.h>
#include <stdint.h>

/* menu_index value the guest parks on for this screen. See
 * HleSmGameState_15_Paused_Async for the full index map. */
enum { SM_MENU_INDEX_OPTIONS = 2 };

/* Called by the guest dispatcher every frame it runs the paused state.
 * Rising edge resets the cursor; falling edge hides the panel. */
void SmPauseOptions_SetActive(bool active);
bool SmPauseOptions_IsActive(void);

/* Guest input -> overlay. Move() is D-pad up/down, Adjust() is left/right on
 * the highlighted row, Activate() is A. */
void SmPauseOptions_Move(int delta);
void SmPauseOptions_Adjust(int delta);
void SmPauseOptions_Activate(void);

/* One-shot intent raised by A on "BACK TO MAP" (or the guest's B). The guest
 * consumes it to leave the screen; this module never touches menu_index or
 * game_state itself. */
bool SmPauseOptions_ConsumeLeaveRequest(void);

/* Composite the panel over an ARGB8888 frame. stride is in pixels. */
void SmPauseOptions_Render(uint32_t *dst, int stride, int width, int height);
