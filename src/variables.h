/* Game-specific RAM variable declarations for Super Metroid.
 *
 * Scaffold: just the framework-protocol variables the host
 * orchestration in sm_rtl.c needs. The recompiled gen code refers to
 * memory by raw address; named regions can be added as needed (the
 * snesrev/sm decomp's src/variables.h is the authoritative RAM map).
 */

#ifndef VARIABLES_H
#define VARIABLES_H

#include "types.h"

/* g_ram is declared by snesrecomp/runner/src/cpu/common_rtl.h. */

/* Host-protocol frame counter (framework-shaped, not game-specific). */
extern uint16 counter_global_frames;

/* Super Metroid's vblank handshake lives at $7E:05B4 (decomp:
 * `waiting_for_nmi`, src/variables.h `g_ram+0x5B4`). WaitForNMI
 * ($80:8338) sets it to 1 and spins until the NMI handler ($80:9583,
 * Vector_NMI) clears it. In the recompiled build there is no mid-frame
 * NMI, so that spin never terminates — WaitForNMI is HLE-replaced by a
 * host-fiber yield (HleSmWaitForNmi -> sm_host_yield in sm_rtl.c). */
#define waiting_for_nmi (*(uint8*)(g_ram + 0x05B4))

/* Pause menu variables (from snesrev/sm decomp) */
#define menu_index (*(uint16*)(g_ram + 0x07E1A))
#define pause_screen_mode (*(uint8*)(g_ram + 0x07F00))
#define pausemenu_button_label_mode (*(uint8*)(g_ram + 0x07F01))
#define newly_held_down_timed_held_input (*(uint16*)(g_ram + 0x07E1C))
#define joypad1_newkeys (*(uint16*)(g_ram + 0x07E1E))
#define game_state (*(uint8*)(g_ram + 0x0998))
#define screen_fade_delay (*(uint16*)(g_ram + 0x07E20))
#define screen_fade_counter (*(uint16*)(g_ram + 0x07E22))
#define pausemenu_start_lr_pressed_highlight_timer (*(uint8*)(g_ram + 0x07F02))
#define pausemenu_equipment_category_item (*(uint8*)(g_ram + 0x07F03))
#define reg_INIDISP (*(uint8*)(g_ram + 0x2100))

/* Button bitmasks */
#define kButton_A       0x8000
#define kButton_B       0x4000
#define kButton_X       0x2000
#define kButton_Y       0x1000
#define kButton_L       0x0800
#define kButton_R       0x0400
#define kButton_Start   0x0200
#define kButton_Up      0x0080
#define kButton_Down    0x0040
#define kButton_Left    0x0020
#define kButton_Right   0x0010

/* Game state values (from snesrev/sm decomp) */
#define kGameState_8_MainGameplay 8
#define kGameState_ExitToLauncher 0xFF  /* custom state for graceful exit */

#endif /* VARIABLES_H */
