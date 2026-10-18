/* gen_stubs.c — hand-written HLE bodies the recompiler is told to call
 * in place of the original asm (via `hle_func <pc> <name>` cfg
 * directives). For Super Metroid the only boot-critical one is the
 * vblank wait.
 */
#include <stdint.h>
#include <stdio.h>
#include "cpu_state.h"
#include "common_rtl.h"
#include "cpu_trace.h"
#include "variables.h"
#include "sm_rtl.h"

/* WaitForNMI ($80:8338). The original routine raises waiting_for_nmi
 * ($7E:05B4) and spins until the NMI handler ($80:9583) clears it. In
 * the recompiled build there is no mid-frame NMI to clear it, so the
 * spin would never terminate. Replace the whole routine with a host
 * yield: raise the same WRAM handshake flag as the ROM, hand control
 * back to RunOneFrameOfGame (which runs the NMI handler and emulates the
 * frame), then continue. The recompiled NMI handler clears the flag; the
 * HLE must not clear it itself or the next NMI observes "not waiting"
 * and skips the queue-draining work.
 *
 * The HLE replaces the WHOLE routine, including its terminating RTL. Under
 * the Option-1 cpu->S ABI the caller's `JSL WaitForNMI` pushed a 3-byte
 * return frame that the real routine's RTL would pop; returning
 * RECOMP_RETURN_NORMAL signals a host-return, which (per _emit_return)
 * only happens AFTER the frame pop. So we must pop the 3-byte frame here —
 * otherwise it is orphaned and the 65816 stack leaks 3 bytes per vblank,
 * underflowing after ~1000 frames (the HdmaObjectHandler "runaway" crash). */
RecompReturn HleSmWaitForNmi(CpuState *cpu) {
  /* Under the LLE loop driver there is no hosting game fiber. Preserve the
   * guest JSL frame and unwind to the interpreter, which resumes at the real
   * WaitForNMI bytes and stops at its asserted wait loop. */
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(
        cpu, ((uint32)cpu->PB << 16) | 0x8338u);
  waiting_for_nmi = 1;
  sm_host_yield();
  cpu->S = (uint16)(cpu->S + 3);  /* emulate RTL: pop the JSL return frame */
  return RECOMP_RETURN_NORMAL;
}

/* $82:E039: inline-parameter VRAM update coroutine. */
RecompReturn HleSmDoorTransitionVramWait(CpuState *cpu) {
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(cpu, 0x82E039u);

  const uint8 saved_db = cpu->DB;
  uint16 ret = cpu_read16(cpu, 0x00, (uint16)(cpu->S + 1));
  uint16 ptr = (uint16)(ret + 1);

  cpu_write16(cpu, cpu->PB, 0x05C0, cpu_read16(cpu, cpu->PB, ptr));
  ptr = (uint16)(ptr + 1);
  cpu_write16(cpu, cpu->PB, 0x05C1, cpu_read16(cpu, cpu->PB, ptr));
  ptr = (uint16)(ptr + 2);
  cpu_write16(cpu, cpu->PB, 0x05BE, cpu_read16(cpu, cpu->PB, ptr));
  ptr = (uint16)(ptr + 2);
  cpu_write16(cpu, cpu->PB, 0x05C3, cpu_read16(cpu, cpu->PB, ptr));
  ptr = (uint16)(ptr + 1);
  cpu_write16(cpu, 0x00, (uint16)(cpu->S + 1), ptr);

  uint16 wait = (uint16)(cpu_read16(cpu, saved_db, 0x05BC) | 0x8000u);
  cpu_write16(cpu, saved_db, 0x05BC, wait);
  cpu->A = wait;
  cpu->_flag_Z = wait == 0;
  cpu->_flag_N = (wait & 0x8000u) != 0;
  cpu->P = (uint8)((cpu->P & ~0x82u) |
                   (cpu->_flag_Z ? 0x02u : 0u) |
                   (cpu->_flag_N ? 0x80u : 0u));

  sm_host_yield();
  wait = cpu_read16(cpu, saved_db, 0x05BC);
  cpu->A = wait;
  cpu->_flag_Z = wait == 0;
  cpu->_flag_N = (wait & 0x8000u) != 0;
  cpu->P = (uint8)((cpu->P & ~0x82u) |
                   (cpu->_flag_Z ? 0x02u : 0u) |
                   (cpu->_flag_N ? 0x80u : 0u));
  cpu->S = (uint16)(cpu->S + 2);  /* emulate RTS */
  return RECOMP_RETURN_NORMAL;
}

RecompReturn HleSmWaitUntilEndOfVblank(CpuState *cpu) {
  /* $80:82C5 only polls HVBJOY, then restores A/P and RTLs. The host
   * scheduler has already crossed the frame boundary, matching the official
   * decomp port's removal of this wait. */
  cpu->S = (uint16)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}

static RecompReturn HleSmJmlIndirect(CpuState *cpu, uint16 ptr_addr,
                                     uint32 site_pc24) {
  uint16 target_pc = cpu_read16(cpu, 0x00, ptr_addr);
  uint8 target_bank = cpu_read8(cpu, 0x00, (uint16)(ptr_addr + 2));
  uint32 target = ((uint32)target_bank << 16) | target_pc;
  uint8 saved_pb = cpu->PB;
  uint8 hrv = cpu->host_return_valid;
  uint16 miss_s = (uint16)(cpu->S + ((hrv == 2 || hrv == 3) ? hrv : 0));
  uint32 dispatch_target = target;

  /* The standard enemy-instruction header is byte-identical in A2/A3.
   * A3's decomp map currently swallows $80ED inside a coarse function, so
   * reuse the correctly bounded A2 AOT body while preserving PB/DB=A3. */
  if (target == 0xA380EDu)
    dispatch_target = 0xA280EDu;

  if (!cpu_dispatch_has_entry(cpu, dispatch_target)) {
    (void)cpu_trace_dispatch_oob(cpu, site_pc24, target);

    /* These helpers implement tail JMLs reached through an enclosing JSL.
     * A missing AOT body must still execute the live ROM handler: its RTL
     * consumes that inherited frame and returns to the compiled caller.
     * The old prototype called cpu_dispatch_pc_from(), whose miss path only
     * restored S and silently skipped all handler side effects. */
    cpu->PB = target_bank;
    /* The pointer has already been resolved; interpreter entry is the
     * handler, not the indirect-jump instruction. */
    RecompReturn r = interp_tier_dispatch_balanced(
        cpu, target, site_pc24, cpu->S, hrv, false);
    cpu->PB = saved_pb;
    return r;
  }

  cpu->PB = target_bank;
  RecompReturn r = cpu_dispatch_pc_from(cpu, dispatch_target, miss_s, site_pc24);
  cpu->PB = saved_pb;
  return r;
}

static RecompReturn HleSmJmpIndirectIndexedTail(CpuState *cpu,
                                                uint16 ptr_base,
                                                uint32 site_pc24) {
  uint16 ptr_addr = (uint16)(ptr_base + cpu->X);
  uint16 target_pc = cpu_read16(cpu, cpu->PB, ptr_addr);
  uint32 target = ((uint32)cpu->PB << 16) | target_pc;
  uint8 hrv = cpu->host_return_valid;
  uint16 miss_s = (uint16)(cpu->S + ((hrv == 2 || hrv == 3) ? hrv : 0));

  if (!cpu_dispatch_has_entry(cpu, target)) {
    (void)cpu_trace_dispatch_oob(cpu, site_pc24, target);
    return interp_tier_dispatch_balanced(
        cpu, target, site_pc24, cpu->S, hrv, false);
  }
  return cpu_dispatch_pc_from(cpu, target, miss_s, site_pc24);
}

RecompReturn HleSmJmpIndirect0FA8X_F25F(CpuState *cpu) {
  return HleSmJmpIndirectIndexedTail(cpu, 0x0FA8, 0x28F25Fu);
}

RecompReturn HleSmJmpIndirect0FA8X_F265(CpuState *cpu) {
  return HleSmJmpIndirectIndexedTail(cpu, 0x0FA8, 0x28F265u);
}

RecompReturn HleSmJmpIndirect0FB2X_E6C5(CpuState *cpu) {
  return HleSmJmpIndirectIndexedTail(cpu, 0x0FB2, 0x23E6C5u);
}

RecompReturn HleSmEnemyFuncC8ADPreserveMx(CpuState *cpu) {
  /* Decomp-backed implementation of $A0:C8AD (EnemyFunc_C8AD).  The AOT
   * body for this routine has leaked processor width state and corrupted
   * KiHunter's enemy index.  Keep this helper local to the proven failing
   * call site until it has been checked against the full attract sequence.
   *
   * The original saves X/Y/P, clears carry in the saved P, and changes that
   * saved carry to one if either sampled block is a non-square slope.  Its
   * final PLX/PLY leave N/Z describing the restored Y value. */
  const uint16 k = cpu->x_flag ? (uint8)cpu->X : cpu->X;
  const uint8 data_bank = cpu->DB;
  const uint8 saved_p = cpu->P;
  const uint16 saved_x = cpu->X;
  const uint16 saved_y = cpu->Y;
  const uint16 x_pos = cpu_read16(cpu, data_bank, (uint16)(0x0F7A + k));
  uint16 y_pos = cpu_read16(cpu, data_bank, (uint16)(0x0F7E + k));
  const uint16 y_height =
      cpu_read16(cpu, data_bank, (uint16)(0x0F84 + k));
  const uint16 room_width = cpu_read16(cpu, data_bank, 0x07A5);
  uint8 result = 0;

  uint16 block = (uint16)((uint8)((uint16)(y_pos + y_height - 1) >> 4) *
                              (uint8)room_width +
                          (x_pos >> 4));
  cpu_write16(cpu, data_bank, 0x0DC4, block);
  uint16 level = cpu_read16(cpu, 0x7F, (uint16)(0x0002 + block * 2));
  uint8 bts = cpu_read8(cpu, 0x7F, (uint16)(0x6402 + block));
  if ((level & 0xF000u) == 0x1000u && (bts & 0x1Fu) >= 5) {
    uint16 temp_dd4 = (uint16)(y_height + y_pos - 1) & 0x000Fu;
    uint16 temp_dd6 = (uint16)(16 * (bts & 0x1Fu));
    result = 1;
    cpu_write16(cpu, data_bank, 0x0DD4, temp_dd4);
    cpu_write16(cpu, data_bank, 0x0DD6, temp_dd6);
    if (!(bts & 0x80u)) {
      uint16 sample_x = (bts & 0x40u) ? (x_pos ^ 0x000Fu) : x_pos;
      uint8 align = cpu_read8(
          cpu, 0x94,
          (uint16)(0x8B2B + temp_dd6 + (sample_x & 0x000Fu)));
      int16_t correction =
          (int16_t)((align & 0x1Fu) - temp_dd4 - 1);
      if (correction < 0) {
        y_pos = (uint16)(y_pos + correction);
        cpu_write16(cpu, data_bank, (uint16)(0x0F7E + k), y_pos);
      }
    }
  }

  block = (uint16)((uint8)((uint16)(y_pos - y_height) >> 4) *
                         (uint8)room_width +
                     (x_pos >> 4));
  cpu_write16(cpu, data_bank, 0x0DC4, block);
  level = cpu_read16(cpu, 0x7F, (uint16)(0x0002 + block * 2));
  bts = cpu_read8(cpu, 0x7F, (uint16)(0x6402 + block));
  if ((level & 0xF000u) == 0x1000u && (bts & 0x1Fu) >= 5) {
    uint16 temp_dd4 =
        (((uint16)(y_pos - y_height) & 0x000Fu) ^ 0x000Fu);
    uint16 temp_dd6 = (uint16)(16 * (bts & 0x1Fu));
    result = 1;
    cpu_write16(cpu, data_bank, 0x0DD4, temp_dd4);
    cpu_write16(cpu, data_bank, 0x0DD6, temp_dd6);
    if (bts & 0x80u) {
      uint16 sample_x = (bts & 0x40u) ? (x_pos ^ 0x000Fu) : x_pos;
      uint8 align = cpu_read8(
          cpu, 0x94,
          (uint16)(0x8B2B + temp_dd6 + (sample_x & 0x000Fu)));
      int16_t correction =
          (int16_t)((align & 0x1Fu) - temp_dd4 - 1);
      if (correction < 0) {
        y_pos = (uint16)(y_pos - correction);
        cpu_write16(cpu, data_bank, (uint16)(0x0F7E + k), y_pos);
      }
    }
  }

  cpu->X = saved_x;
  cpu->Y = saved_y;
  cpu->P = (uint8)((saved_p & ~0x83u) | (result ? 0x01u : 0u));
  if ((cpu->x_flag ? (uint8)saved_y : saved_y) == 0)
    cpu->P |= 0x02u;
  if (cpu->x_flag ? ((saved_y & 0x0080u) != 0)
                  : ((saved_y & 0x8000u) != 0))
    cpu->P |= 0x80u;
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16)(cpu->S + 3);  /* emulate the routine's RTL */
  return RECOMP_RETURN_NORMAL;
}


#define SM_JML1784_WRAPPER(name, site) \
  RecompReturn name(CpuState *cpu) { return HleSmJmlIndirect(cpu, 0x1784, site); }
#define SM_JML178C_WRAPPER(name, site) \
  RecompReturn name(CpuState *cpu) { return HleSmJmlIndirect(cpu, 0x178C, site); }

SM_JML178C_WRAPPER(HleSmJmlIndirect178C_88CB, 0x2088CBu)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_8BF0, 0x208BF0u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_90A3, 0x2090A3u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_9420, 0x209420u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_9B4C, 0x209B4Cu)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_9D20, 0x209D20u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_9E97, 0x209E97u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_A025, 0x20A025u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_A140, 0x20A140u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_A233, 0x20A233u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_A303, 0x20A303u)
SM_JML1784_WRAPPER(HleSmJmlIndirect1784_A3AC, 0x20A3ACu)

#undef SM_JML1784_WRAPPER
#undef SM_JML178C_WRAPPER

/* ──────────────────────────────────────────────────────────────────────
 * Extended Pause Menu — 3rd "Game Options" Tab
 *
 * Replaces GameState_15_Paused_Async (0x8290E8), HandlePauseScreenLrInput
 * (0x82A50C), and HandlePauseScreenStart (0x82A5B7) to add a third top-level
 * tab alongside Map and Equipment.
 *
 * menu_index (RAM 0x07E1A) values:
 *   0 = Map Screen
 *   1 = Equipment Screen
 *   2 = Game Options Screen (NEW)
 *   3 = Map->Equipment Load (orig)
 *   4 = Map->Equipment FadeIn (orig)
 *   5 = Equipment->Map Load (orig)
 *   6 = Equipment->Map FadeIn (orig)
 *   7 = Equipment->Options Load (NEW)
 *   8 = Equipment->Options FadeIn (NEW)
 *   9 = Options->Map Load (NEW)
 *  10 = Options->Map FadeIn (NEW)
 *  11 = Options->Map FadeIn Complete -> back to Map (NEW)
 *  12 = Map->Options Load (NEW)
 *  13 = Map->Options FadeIn (NEW)
 * ────────────────────────────────────────────────────────────────────── */

static uint16 s_game_options_cursor;

/* Forward declarations for original functions we still call for transitions.
 * These are resolved at link time via funcs.h (generated from the cfg). */
extern void PauseMenu_0_MapScreen(void);
extern void PauseMenu_1_EquipmentScreen(void);
extern void PauseMenu_3_MapToEquipment_Load(void);
extern void PauseMenu_4_MapToEquipment_FadeIn(void);
extern void PauseMenu_6_EquipmentToMap_Load(void);
extern void PauseMenu_7_EquipmentToMap_FadeIn(void);
extern void HandlePauseScreenPaletteAnimation(void);
extern void HandleHudTilemap(void);
extern void ReleaseButtonsFilter(uint16);
extern void DrawPauseMenuDuringFadeIn(void);
extern void HandleFadeIn(void);
extern void HandleFadeOut(void);
extern void SetPauseScreenButtonLabelPalettes(void);
extern void UpdatePauseMenuLRStartVramTilemap(void);
extern void QueueSfx1_Max6(uint16);
extern void SaveSRAM(void);

/* Forward declarations for new HLE functions (defined below) */
static void HleSmGameOptionsScreen_Impl(void);
static void HleSmEquipmentToOptions_Load(void);
static void HleSmEquipmentToOptions_FadeIn(void);
static void HleSmMapToOptions_Load(void);
static void HleSmMapToOptions_FadeIn(void);
static void HleSmGracefulExit(void);

/* Simple text drawing helper - uses the existing VRAM tilemap approach */
static void DrawGameOptionsText(const char *text, uint16 x, uint16 y, uint16 color) {
  /* For now, reuse the existing EquipmentScreenDrawItemSelector approach
   * but we need a simpler way. The game uses tilemaps in VRAM for text.
   * We'll write directly to the BG1 tilemap area used by the pause menu. */
  (void)text; (void)x; (void)y; (void)color;
  /* TODO: Implement proper text rendering using the game's font tiles.
   * For MVP, we'll use the existing item selector highlight as a cursor
   * and rely on the pre-rendered "GAME OPTIONS" tilemap we set up. */
}

RecompReturn HleSmGameState_15_Paused_Async(CpuState *cpu) {
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(cpu, 0x8290E8u);

  ReleaseButtonsFilter(3);

  uint16 idx = menu_index;

  if (idx == 0) {
    PauseMenu_0_MapScreen();
  } else if (idx == 1) {
    PauseMenu_1_EquipmentScreen();
  } else if (idx == 2) {
    /* Game Options screen - main loop */
    HleSmGameOptionsScreen_Impl();
  } else if (idx == 3) {
    PauseMenu_3_MapToEquipment_Load();
  } else if (idx == 4) {
    PauseMenu_4_MapToEquipment_FadeIn();
  } else if (idx == 5) {
    HandleFadeOut();
    if (reg_INIDISP == 0x80) {
      ++menu_index;
    }
  } else if (idx == 6) {
    PauseMenu_6_EquipmentToMap_Load();
  } else if (idx == 7) {
    PauseMenu_7_EquipmentToMap_FadeIn();
  } else if (idx == 8) {
    /* Equipment -> Options transition: Load */
    HleSmEquipmentToOptions_Load();
  } else if (idx == 9) {
    /* Equipment -> Options transition: FadeIn */
    HleSmEquipmentToOptions_FadeIn();
  } else if (idx == 10) {
    /* Options -> Map transition: Load */
    HandleFadeOut();
    if (reg_INIDISP == 0x80) {
      ++menu_index;
    }
  } else if (idx == 11) {
    /* Options -> Map transition: FadeIn (reuse Equipment->Map fade in) */
    PauseMenu_7_EquipmentToMap_FadeIn();
    menu_index = 0;
  } else if (idx == 12) {
    /* Map -> Options transition: Load */
    HleSmMapToOptions_Load();
  } else if (idx == 13) {
    /* Map -> Options transition: FadeIn */
    HleSmMapToOptions_FadeIn();
  }

  HandleHudTilemap();
  HandlePauseScreenPaletteAnimation();

  cpu->S = (uint16)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}

void HleSmGameOptionsScreen_Impl(void) {
  /* Handle L/R for 3-way tab cycling */
  if ((newly_held_down_timed_held_input & kButton_L) != 0) {
    if (!pausemenu_button_label_mode) return;
    uint16 idx = menu_index;
    if (idx == 0) { menu_index = 12; }          /* Map -> Options */
    else if (idx == 2) { menu_index = 9; }      /* Options -> Map (then R to Equipment) */
    else if (idx == 1) { menu_index = 5; }      /* Equipment -> Map (existing) */
    pausemenu_button_label_mode = 0;
    SetPauseScreenButtonLabelPalettes();
    return;
  }
  if ((newly_held_down_timed_held_input & kButton_R) != 0) {
    if (!pausemenu_button_label_mode) return;
    uint16 idx = menu_index;
    if (idx == 0) { menu_index = 2; }           /* Map -> Equipment (existing) */
    else if (idx == 1) { menu_index = 7; }      /* Equipment -> Options */
    else if (idx == 2) { menu_index = 9; }      /* Options -> Map */
    pausemenu_button_label_mode = 0;
    SetPauseScreenButtonLabelPalettes();
    return;
  }

  /* Handle Start to unpause */
  if ((newly_held_down_timed_held_input & kButton_Start) != 0) {
    QueueSfx1_Max6(0x38);
    screen_fade_delay = 1;
    screen_fade_counter = 1;
    uint16 v0 = pausemenu_button_label_mode;
    pausemenu_button_label_mode = 1;
    SetPauseScreenButtonLabelPalettes();
    pausemenu_button_label_mode = v0;
    pausemenu_start_lr_pressed_highlight_timer = 11;
    ++game_state;
    return;
  }

  /* D-pad navigation for menu cursor */
  if ((joypad1_newkeys & kButton_Up) != 0) {
    s_game_options_cursor = (s_game_options_cursor + 4) % 5;
    QueueSfx1_Max6(0x37);
  }
  if ((joypad1_newkeys & kButton_Down) != 0) {
    s_game_options_cursor = (s_game_options_cursor + 1) % 5;
    QueueSfx1_Max6(0x37);
  }

  /* A button selection */
  if ((joypad1_newkeys & kButton_A) != 0) {
    QueueSfx1_Max6(0x38);
    switch (s_game_options_cursor) {
      case 0: HleSmGracefulExit(); break;
      case 1: /* Toggle FPS - reuse existing FPS mod */ break;
      case 2: /* Toggle Widescreen - reuse existing widescreen mod */ break;
      case 3: /* Audio Settings - placeholder */ break;
      case 4: menu_index = 0; s_game_options_cursor = 0; break; /* Back to Map */
    }
  }

  pause_screen_mode = 2;  /* Custom mode for our screen */
}

void HleSmEquipmentToOptions_Load(void) {
  /* Fade out equipment screen, prepare options screen */
  HandleFadeOut();
  if (reg_INIDISP == 0x80) {
    ++menu_index;
  }
}

void HleSmEquipmentToOptions_FadeIn(void) {
  /* Fade in options screen - reuse equipment fade-in logic but for our screen */
  DrawPauseMenuDuringFadeIn();
  HandleFadeIn();
  if (reg_INIDISP == 15) {
    screen_fade_delay = 0;
    screen_fade_counter = 0;
    ++menu_index;
  }
}

void HleSmMapToOptions_Load(void) {
  /* Fade out map screen, prepare options screen */
  HandleFadeOut();
  if (reg_INIDISP == 0x80) {
    ++menu_index;
  }
}

void HleSmMapToOptions_FadeIn(void) {
  /* Fade in options screen */
  DrawPauseMenuDuringFadeIn();
  HandleFadeIn();
  if (reg_INIDISP == 15) {
    screen_fade_delay = 0;
    screen_fade_counter = 0;
    menu_index = 2;  /* Now on Options screen */
  }
}

void HleSmGracefulExit(void) {
  /* Save SRAM and exit to launcher */
  SaveSRAM();
  screen_fade_delay = 1;
  screen_fade_counter = 1;
  game_state = kGameState_ExitToLauncher;
}

RecompReturn HleSmHandlePauseScreenLrInput(CpuState *cpu) {
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(cpu, 0x82A50Cu);

  /* The 3-way cycling is now handled in HleSmGameOptionsScreen_Impl
   * but we keep this for when on Map/Equipment screens */
  cpu->S = (uint16)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn HleSmHandlePauseScreenStart(CpuState *cpu) {
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(cpu, 0x82A5B7u);

  if ((newly_held_down_timed_held_input & kButton_Start) != 0) {
    QueueSfx1_Max6(0x38);
    screen_fade_delay = 1;
    screen_fade_counter = 1;
    uint16 v0 = pausemenu_button_label_mode;
    pausemenu_button_label_mode = 1;
    SetPauseScreenButtonLabelPalettes();
    pausemenu_button_label_mode = v0;
    pausemenu_start_lr_pressed_highlight_timer = 11;
    ++game_state;
  }
  UpdatePauseMenuLRStartVramTilemap();

  cpu->S = (uint16)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}
