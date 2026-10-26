/* gen_stubs.c — hand-written HLE bodies the recompiler is told to call
 * in place of the original asm (via `hle_func <pc> <name>` cfg
 * directives). For Super Metroid the only boot-critical one is the
 * vblank wait.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu_state.h"
#include "common_rtl.h"
#include "cpu_trace.h"
#include "variables.h"
#include "sm_rtl.h"

/* Wait replacements unwind compiled calls to the interruptible scheduler.
 * The original ROM bytes own the WRAM handshake and return-frame pop; the
 * scheduler injects NMI at the polling boundary and resumes that guest PC. */
static RecompReturn sm_scheduler_yield(CpuState *cpu, uint32_t pc) {
  if (!interp_bridge_in_lle_scheduler()) {
    fprintf(stderr, "[sm_rtl] wait $%06X called outside the frame scheduler\n", (unsigned)pc);
    abort();
  }
  return interp_bridge_lle_yield_unwind(cpu, pc);
}
RecompReturn HleSmWaitForNmi(CpuState *cpu) {
  /* Preserve the JSL frame and execute the real polling bytes under the
   * interruptible scheduler; every runtime policy uses this continuation. */
  return sm_scheduler_yield(cpu, ((uint32)cpu->PB << 16) | 0x8338u);
}

/* $82:E039: inline-parameter VRAM update coroutine. */
RecompReturn HleSmDoorTransitionVramWait(CpuState *cpu) {
  return sm_scheduler_yield(cpu, 0x82E039u);
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
