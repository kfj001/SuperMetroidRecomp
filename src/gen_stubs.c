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
#include "sm_pause_options.h"
#include "host_report.h"

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
 * Extended Pause Menu — 3rd "Game Options" tab
 *
 * Replaces GameState_15_Paused_Async (0x8290E8), HandlePauseScreenLrInput
 * (0x82A50C) and HandlePauseScreenStart (0x82A5B7) to add a third top-level
 * tab alongside Map and Equipment.
 *
 * The ROM's own dispatcher (kPauseMenuFuncs, 0x8290FF) uses menu_index 0-7.
 * This HLE dispatch keeps every one of those at its vanilla meaning — the
 * fade-out / Load / FadeIn trios that carry Map<->Equipment are the ROM's
 * own PauseMenu_2..7, unmodified — and appends two indices on top:
 *
 *   menu_index:
 *     0   Map screen                 (PauseMenu_0_MapScreen)
 *     1   Equipment screen           (PauseMenu_1_EquipmentScreen)
 *     2   Game Options screen        (NEW: HleSmOptionsScreen, host overlay)
 *     3   Map->Equipment fade-out    (PauseMenu_2)
 *     4   Map->Equipment Load        (PauseMenu_3_MapToEquipment_Load)
 *     5   Map->Equipment FadeIn      (PauseMenu_4_MapToEquipment_FadeIn)
 *     6   Equipment->Map fade-out    (PauseMenu_5)
 *     7   Equipment->Map Load        (PauseMenu_6_EquipmentToMap_Load)
 *     8   Equipment->Map FadeIn      (PauseMenu_7_EquipmentToMap_FadeIn)
 *     9   generic fade-out -> s_pause_pending   (NEW)
 *
 * Tab ring: Map(0) -> Equipment(1) -> Options(2) -> Map(0). R walks it
 * forward, L walks it backward, both wrapping at the ends.
 *
 * Every transition that touches the Options tab fades through index 9,
 * which fades the current screen out and then jumps to s_pause_pending.
 * Going IN, the pending target is 2 and nothing is drawn for it — the tab
 * is a host overlay, so the guest has no screen to load. Going back OUT,
 * the pending target is one of the ROM's own Load states (4 for Equipment,
 * 7 for Map), so the destination is set up and faded in by untouched
 * vanilla code. pausemenu_button_label_mode is always set to the
 * *destination* before the fade, because that is precisely what
 * PauseMenu_4/7 read to pick their final menu_index (0 for Map, non-zero
 * for Equipment): vanilla's own trick, reused rather than reimplemented.
 *
 * Why the Options screen has no guest rendering at all: Super Metroid's
 * pause-menu text is baked. The letters exist only as pre-composed tilemap
 * blocks that EquipmentScreenTransferBG1Tilemap DMAs out of the ROM into
 * ram3800; there is no string compositor anywhere to hang new text off, and
 * the only digit path is tiles 2052+n. So the third tab draws through the
 * framework's shared 8x8-font primitives (src/sm_pause_options.c) — the
 * same ones the save-state browser uses — at the draw_frame seam, and the
 * guest owns nothing but navigation.
 *
 * Return-frame discipline (what the previous revision got wrong for two of
 * these three): the generated caller pushes the guest return frame and NEVER
 * pops it on the normal path, so the HLE must pop exactly what the real
 * routine's RTS/RTL would have.
 *   HandlePauseScreenLrInput and HandlePauseScreenStart are JSR-called —
 *   the generated callers push exactly 2 bytes and set
 *   host_return_valid = 2 — so they emulate RTS with S += 2. They were doing
 *   += 3, leaking a byte per call into the pause routine's own frame.
 *   GameState_15_Paused_Async is JSL-called: kGameStateFuncs is a table of
 *   long-call coroutine targets (GameState_14_Paused_Async, the untouched
 *   twin of the one replaced here, emits CPU_TR_RTL and restores PB), so it
 *   emulates RTL with S += 3, which is correct and unchanged.
 * ────────────────────────────────────────────────────────────────────── */

/* menu_index 9's destination; set by HleSmStartFadeTo. */
static uint16 s_pause_pending;

/* ── Paired guest-call dispatch ─────────────────────────────────────────
 * The pause helpers below drive real ROM routines (the palette variants, the
 * sfx queue, the PauseMenu_* screens, HandleFadeOut/EnableNMI, and the HUD /
 * palette-animation ticks) as *callees*. Per HLE_FUNC.md a helper must not
 * call a generated `void` alias: those push no return frame and abort() on
 * any non-local return. Instead each call pushes the frame the original
 * JSR/JSL would push, dispatches the live (m,x) AOT body (falling back to
 * the interpreter tier on a miss), then mirrors a generated call site:
 * on a non-local return, one decrement for the frame it pushed. The
 * caller's PB is saved/restored around the call, exactly as a generated
 * JSL site does. */
static RecompReturn HleCallGuest(CpuState *cpu, uint32 pc24, uint32 site,
                                 uint8 frame, uint8 exec_pb) {
  const uint8 saved_pb = cpu->PB;
  const uint8 saved_hrv = cpu->host_return_valid;
  if (frame == 3)
    cpu_push_jsl_return_frame(cpu);
  else
    cpu_push_jsr_return_frame(cpu);
  cpu->PB = exec_pb;
  RecompReturn r = cpu_dispatch_call_pc_pushed(cpu, pc24, site, frame, NULL);
  cpu->PB = saved_pb;
  cpu->host_return_valid = saved_hrv;
  if (r != RECOMP_RETURN_NORMAL)
    return (RecompReturn)((int)r - 1);
  return RECOMP_RETURN_NORMAL;
}

/* The three palette variants the cfg emits for $02:A628/A6DF/A796. The
 * un-numbered dispatcher that picks between them ($02:A615) was absorbed into
 * HighlightPauseScreenButton's cfg range, so it is reproduced here from the
 * sm decomp (kSetPauseScreenButtonLabelPalettes_Funcs[mode]()). */
static RecompReturn SetPauseScreenButtonLabelPalettes(CpuState *cpu,
                                                      uint32 site) {
  switch (pausemenu_button_label_mode) {
    case 0: return HleCallGuest(cpu, 0x02A796u, site, 2, cpu->PB);
    case 1: return HleCallGuest(cpu, 0x02A6DFu, site, 2, cpu->PB);
    case 2: return HleCallGuest(cpu, 0x02A628u, site, 2, cpu->PB);
    default: return RECOMP_RETURN_NORMAL;
  }
}

/* These helper routines take their argument in the accumulator (the ROM
 * calling convention), which the recompiled bodies read back out of the CPU
 * state — so seed cpu->A before the call. Only the low byte is written:
 * QueueSfx1_Max6 XBAs it up into place itself, matching the ROM's
 * QueueSfx1_Internal(a << 8 | 6). */
static RecompReturn HleSmQueueSfx1(CpuState *cpu, uint32 site, uint16 id) {
  cpu->A = (uint16)((cpu->A & 0xFF00) | (id & 0xFF));
  return HleCallGuest(cpu, 0x009049u, site, 3, 0x80u);  /* QueueSfx1_Max6 */
}

/* Park the pause menu on a new tab. The caller sets
 * pausemenu_button_label_mode and calls SetPauseScreenButtonLabelPalettes;
 * this only arms the fade and its destination. */
static void HleSmStartFadeTo(uint16 pending, uint16 label_mode) {
  s_pause_pending = pending;
  pausemenu_button_label_mode = label_mode;
  menu_index = 9;
}

static RecompReturn HleSmGenericFadeOut(CpuState *cpu, uint32 site) {
  RecompReturn r = HleCallGuest(cpu, 0x008924u, site, 3, 0x80u);  /* HandleFadeOut */
  if (r != RECOMP_RETURN_NORMAL)
    return r;
  if (reg_INIDISP == 0x80) {
    r = HleCallGuest(cpu, 0x00834Bu, site, 3, 0x80u);  /* EnableNMI */
    if (r != RECOMP_RETURN_NORMAL)
      return r;
    screen_fade_delay = 0;
    screen_fade_counter = 0;
    menu_index = s_pause_pending;
  }
  return RECOMP_RETURN_NORMAL;
}

/* Shared by HleSmHandlePauseScreenLrInput (Map and Equipment) and by the
 * Options screen below, which does not go through PauseMenu_0/1 and so never
 * reaches the recompiled HandlePauseScreenLR. */
static RecompReturn HleSmPauseMenuLrCycle(CpuState *cpu, uint32 site) {
  int which;   /* -1 = L, +1 = R */
  /* Check both input sources: the original game uses
   * newly_held_down_timed_held_input for L/R, but the host may only update
   * joypad1_newkeys (used by Up/Down/Left/Right/A/B on the Options screen). */
  uint16 lr_input = newly_held_down_timed_held_input | joypad1_newkeys;
  host_report_breadcrumb("HleSmPauseMenuLrCycle: lr_input=0x%04X (newly=0x%04X joypad=0x%04X) menu_index=%d",
           lr_input, newly_held_down_timed_held_input, joypad1_newkeys, menu_index);
  if ((lr_input & kButton_L) != 0)
    which = -1;
  else if ((lr_input & kButton_R) != 0)
    which = 1;
  else
    return RECOMP_RETURN_NORMAL;

  host_report_breadcrumb("HleSmPauseMenuLrCycle: which=%d, menu_index=%d", which, menu_index);
  switch (menu_index) {
    case 0:  /* Map */
      if (which < 0) {
        HleSmStartFadeTo(SM_MENU_INDEX_OPTIONS, 1);  /* wrap past Equipment */
      } else {
        menu_index = 3;                              /* PauseMenu_2 */
        pausemenu_button_label_mode = 2;
      }
      break;
    case 1:  /* Equipment */
      if (which < 0) {
        menu_index = 6;                              /* PauseMenu_5 */
        pausemenu_button_label_mode = 0;
      } else {
        HleSmStartFadeTo(SM_MENU_INDEX_OPTIONS, 1);
      }
      break;
    case SM_MENU_INDEX_OPTIONS:
      if (which < 0) {
        HleSmStartFadeTo(4, 2);   /* -> Equipment, via the ROM's Load state */
      } else {
        HleSmStartFadeTo(7, 0);   /* -> Map, via the ROM's Load state */
      }
      break;
    default:
      return RECOMP_RETURN_NORMAL;
  }

  RecompReturn r = SetPauseScreenButtonLabelPalettes(cpu, site);
  if (r != RECOMP_RETURN_NORMAL)
    return r;
  pausemenu_start_lr_pressed_highlight_timer = 5;
  pausemenu_shoulder_button_highlight = (which < 0) ? 1 : 2;
  return HleSmQueueSfx1(cpu, site, 0x38);
}

/* menu_index 2. The screen itself is drawn host-side; here there is only
 * navigation, plus the Start-to-resume path the ROM keeps in
 * HandlePauseScreenStart and which this screen does not reach. */
static RecompReturn HleSmOptionsScreen(CpuState *cpu, uint32 site) {
  RecompReturn r = HleSmPauseMenuLrCycle(cpu, site);
  if (r != RECOMP_RETURN_NORMAL)
    return r;
  if (menu_index != SM_MENU_INDEX_OPTIONS)
    return RECOMP_RETURN_NORMAL;   /* a transition started; leave the rest of the frame to it */

  if ((newly_held_down_timed_held_input & kButton_Start) != 0) {
    r = HleSmQueueSfx1(cpu, site, 0x38);
    if (r != RECOMP_RETURN_NORMAL)
      return r;
    screen_fade_delay = 1;
    screen_fade_counter = 1;
    uint16 v0 = pausemenu_button_label_mode;
    pausemenu_button_label_mode = 1;
    r = SetPauseScreenButtonLabelPalettes(cpu, site);
    if (r != RECOMP_RETURN_NORMAL)
      return r;
    pausemenu_button_label_mode = v0;
    pausemenu_start_lr_pressed_highlight_timer = 11;
    SmPauseOptions_SetActive(false);
    ++game_state;
    return RECOMP_RETURN_NORMAL;
  }

  if ((joypad1_newkeys & kButton_Up) != 0) {
    SmPauseOptions_Move(-1);
    r = HleSmQueueSfx1(cpu, site, 0x37);
    if (r != RECOMP_RETURN_NORMAL) return r;
  }
  if ((joypad1_newkeys & kButton_Down) != 0) {
    SmPauseOptions_Move(1);
    r = HleSmQueueSfx1(cpu, site, 0x37);
    if (r != RECOMP_RETURN_NORMAL) return r;
  }
  if ((joypad1_newkeys & kButton_Left) != 0) {
    SmPauseOptions_Adjust(-1);
    r = HleSmQueueSfx1(cpu, site, 0x37);
    if (r != RECOMP_RETURN_NORMAL) return r;
  }
  if ((joypad1_newkeys & kButton_Right) != 0) {
    SmPauseOptions_Adjust(1);
    r = HleSmQueueSfx1(cpu, site, 0x37);
    if (r != RECOMP_RETURN_NORMAL) return r;
  }
  if ((joypad1_newkeys & kButton_A) != 0) {
    SmPauseOptions_Activate();
    r = HleSmQueueSfx1(cpu, site, 0x38);
    if (r != RECOMP_RETURN_NORMAL) return r;
  }

  if (SmPauseOptions_ConsumeLeaveRequest() || (joypad1_newkeys & kButton_B) != 0)
    HleSmStartFadeTo(7, 0);   /* Options -> Map, via the ROM's Load state */
  return RECOMP_RETURN_NORMAL;
}

/* ── LLE pause path ─────────────────────────────────────────────────────
 * Under the LLE scheduler (the default: SNESRECOMP_EXECUTION_MODE_OFF) the
 * scheduler owns the frame and its master-cycle deadline, and a helper
 * entered from it must not re-enter the guest dispatcher (docs/HLE_FUNC.md,
 * docs/LLE_SCHEDULER.md). The LLE path therefore does host-only work and then
 * hands the frame back to the interpreter at the routine's real entry, which
 * runs the vanilla ROM bytes — ReleaseButtonsFilter(3), the pause screen, the
 * HUD tick and the palette animation included.
 *
 * The panel is opaque and the ROM has no third screen, so the guest is parked
 * on its own Map screen (menu_index 0) while the panel is up and the host tab
 * is host state (SmPauseOptions_IsActive). Tab ring, R forward:
 *   Map -> Equipment -> Options -> Map; L walks it backward.
 */

/* Swallow a press so the ROM's ReleaseButtonsFilter(3) computes no newly-held
 * edge for it. A newly-pressed button is also in joypad1_lastkeys (it is held
 * this frame), so clearing both is exact. */
static void HleSmPauseConsumePad(uint16 mask) {
  joypad1_newkeys &= (uint16)~mask;
  joypad1_lastkeys &= (uint16)~mask;
  newly_held_down_timed_held_input &= (uint16)~mask;
}

/* Drive the host panel for one paused frame. `pressed` is the frame's
 * newly-pressed set in the ROM's button bits (joypad1_newkeys in LLE; the
 * already-filtered input in HLE). */
static void HleSmPauseHostTick(uint16 pressed) {
  host_report_breadcrumb("HleSmPauseHostTick: pressed=0x%04X menu_index=%d active=%d",
           pressed, menu_index, SmPauseOptions_IsActive());

  if (SmPauseOptions_IsActive()) {
    uint16 used = 0;
    if (pressed & kButton_R) {
      /* Options -> Map (forward): run Equipment->Map fade sequence */
      SmPauseOptions_SetActive(false);
      menu_index = 6;                          /* PauseMenu_5: fade out */
      pausemenu_button_label_mode = 0;         /* destination = Map */
      used |= kButton_R;
    } else if (pressed & kButton_L) {
      /* Options -> Equipment (backward): run Map->Equipment fade sequence */
      SmPauseOptions_SetActive(false);
      menu_index = 3;                          /* PauseMenu_2: fade out */
      pausemenu_button_label_mode = 2;         /* destination = Equipment */
      used |= kButton_L;
    } else {
      if (pressed & kButton_Up)    { SmPauseOptions_Move(-1);   used |= kButton_Up; }
      if (pressed & kButton_Down)  { SmPauseOptions_Move(1);    used |= kButton_Down; }
      if (pressed & kButton_Left)  { SmPauseOptions_Adjust(-1); used |= kButton_Left; }
      if (pressed & kButton_Right) { SmPauseOptions_Adjust(1);  used |= kButton_Right; }
      if (pressed & kButton_A)     { SmPauseOptions_Activate(); used |= kButton_A; }
      if (SmPauseOptions_ConsumeLeaveRequest() || (pressed & kButton_B)) {
        SmPauseOptions_SetActive(false);
        menu_index = 6;                        /* Options -> Map via Equipment->Map transition */
        pausemenu_button_label_mode = 0;
        used |= kButton_B;
      }
    }
    if (used)
      HleSmPauseConsumePad(used);
    if (SmPauseOptions_IsActive())
      menu_index = 0;                          /* keep the ROM on its Map screen */
  } else if ((menu_index == 1 && (pressed & kButton_R)) ||
             (menu_index == 0 && (pressed & kButton_L))) {
    SmPauseOptions_SetActive(true);            /* Map/Equipment -> Options */
    HleSmPauseConsumePad(kButton_R | kButton_L);
    menu_index = 0;                            /* ROM renders its Map under the panel */
  }
}

RecompReturn HleSmGameState_15_Paused_Async(CpuState *cpu) {
  const uint32 site = 0x8290E8u;

  /* LLE: host-only work, then yield to the interpreter at the real entry. */
  if (interp_bridge_in_lle_scheduler()) {
    HleSmPauseHostTick(joypad1_newkeys);
    return interp_bridge_lle_yield_unwind(cpu, 0x8290E8u);
  }

  host_report_breadcrumb("HleSmGameState_15_Paused_Async ENTRY: menu_index=%d, game_state=%d", menu_index, game_state);

  /* Vanilla: ReleaseButtonsFilter(3) — the argument rides in A. It computes
   * newly_held_down_timed_held_input, which every input read below needs. */
  cpu->A = 3;
  RecompReturn r = HleCallGuest(cpu, 0x008146u, site, 3, 0x80u);
  if (r != RECOMP_RETURN_NORMAL)
    return r;

  /* Arm the panel before dispatch so the Options screen can read its own
   * cursor this frame. */
  SmPauseOptions_SetActive(menu_index == SM_MENU_INDEX_OPTIONS);

  host_report_breadcrumb("HleSmGameState_15_Paused_Async: menu_index=%d, game_state=%d, SmPauseOptions_IsActive=%d",
           menu_index, game_state, SmPauseOptions_IsActive());

  switch (menu_index) {
    case 0:  r = HleCallGuest(cpu, 0x029120u, site, 2, cpu->PB); break;  /* PauseMenu_0_MapScreen */
    case 1:  /* PauseMenu_1_EquipmentScreen; also re-check L/R at this level */
      r = HleCallGuest(cpu, 0x029142u, site, 2, cpu->PB);
      if (r == RECOMP_RETURN_NORMAL)
        r = HleSmPauseMenuLrCycle(cpu, site);
      break;
    case 2:  r = HleSmOptionsScreen(cpu, site); break;
    case 3:  r = HleCallGuest(cpu, 0x029156u, site, 2, cpu->PB); break;  /* PauseMenu_2 */
    case 4:  r = HleCallGuest(cpu, 0x0291ABu, site, 2, cpu->PB); break;  /* PauseMenu_3_MapToEquipment_Load */
    case 5:  r = HleCallGuest(cpu, 0x029231u, site, 2, cpu->PB); break;  /* PauseMenu_4_MapToEquipment_FadeIn */
    case 6:  r = HleCallGuest(cpu, 0x029186u, site, 2, cpu->PB); break;  /* PauseMenu_5 */
    case 7:  r = HleCallGuest(cpu, 0x0291D7u, site, 2, cpu->PB); break;  /* PauseMenu_6_EquipmentToMap_Load */
    case 8:  r = HleCallGuest(cpu, 0x029200u, site, 2, cpu->PB); break;  /* PauseMenu_7_EquipmentToMap_FadeIn */
    case 9:  r = HleSmGenericFadeOut(cpu, site); break;
    default: menu_index = 0; r = RECOMP_RETURN_NORMAL; break;   /* e.g. a stale save state: recover */
  }
  if (r != RECOMP_RETURN_NORMAL)
    return r;

  /* Re-evaluate: a transition may have started above. */
  SmPauseOptions_SetActive(menu_index == SM_MENU_INDEX_OPTIONS);

  host_report_breadcrumb("After switch: menu_index=%d, SmPauseOptions_IsActive=%d",
           menu_index, SmPauseOptions_IsActive());

  r = HleCallGuest(cpu, 0x009B44u, site, 3, 0x80u);  /* HandleHudTilemap */
  if (r != RECOMP_RETURN_NORMAL)
    return r;
  r = HleCallGuest(cpu, 0x02A92Bu, site, 2, cpu->PB);  /* HandlePauseScreenPaletteAnimation */
  if (r != RECOMP_RETURN_NORMAL)
    return r;

  cpu->S = (uint16)(cpu->S + 3);  /* emulate RTL: kGameStateFuncs is JSL */
  return RECOMP_RETURN_NORMAL;
}

RecompReturn HleSmHandlePauseScreenLrInput(CpuState *cpu) {
  const uint32 site = 0x82A50Cu;

  /* LLE: the host tick in HleSmGameState_15_Paused_Async has already claimed
   * or suppressed this frame's press, so let the ROM's own handler run. The
   * replacement must not execute inside the LLE scheduler. */
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(cpu, 0x82A50Cu);

  host_report_breadcrumb("HleSmHandlePauseScreenLrInput: lr_input=0x%04X, menu_index=%d",
           (newly_held_down_timed_held_input | joypad1_newkeys), menu_index);

  RecompReturn r = HleSmPauseMenuLrCycle(cpu, site);
  if (r != RECOMP_RETURN_NORMAL)
    return r;

  cpu->S = (uint16)(cpu->S + 2);  /* emulate RTS: the caller JSRs */
  return RECOMP_RETURN_NORMAL;
}

/* Faithful to the ROM's $02:A5B7 — kept as HLE only so its sfx and palette
 * calls go through the corrected accumulator ABI. */
RecompReturn HleSmHandlePauseScreenStart(CpuState *cpu) {
  const uint32 site = 0x82A5B7u;

  /* LLE: resume interpreting the ROM's own $02:A5B7 (see the note on
   * HleSmHandlePauseScreenLrInput). */
  if (interp_bridge_in_lle_scheduler())
    return interp_bridge_lle_yield_unwind(cpu, 0x82A5B7u);

  if ((newly_held_down_timed_held_input & kButton_Start) != 0) {
    RecompReturn r = HleSmQueueSfx1(cpu, site, 0x38);
    if (r != RECOMP_RETURN_NORMAL)
      return r;
    screen_fade_delay = 1;
    screen_fade_counter = 1;
    uint16 v0 = pausemenu_button_label_mode;
    pausemenu_button_label_mode = 1;
    r = SetPauseScreenButtonLabelPalettes(cpu, site);
    if (r != RECOMP_RETURN_NORMAL)
      return r;
    pausemenu_button_label_mode = v0;
    pausemenu_start_lr_pressed_highlight_timer = 11;
    ++game_state;
  }

  RecompReturn r = HleCallGuest(cpu, 0x02A84Du, site, 2, cpu->PB);  /* UpdatePauseMenuLRStartVramTilemap */
  if (r != RECOMP_RETURN_NORMAL)
    return r;

  cpu->S = (uint16)(cpu->S + 2);  /* emulate RTS: the caller JSRs */
  return RECOMP_RETURN_NORMAL;
}
