/*
 * This file is part of theodore (https://github.com/Zlika/theodore).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/* Non-blocking debugger API for GUI frontends (MO5/MO6/PC128).
 *
 * Unlike debugger.c, nothing here touches stdin/stdout. Breakpoints set a
 * halted state; while halted, retro_run() only re-presents the last frame and
 * no instruction is executed. The frontend inspects state with the functions
 * below and then calls theodore_dbg_resume() or theodore_dbg_step().
 *
 * PC breakpoints stop BEFORE the instruction at that address executes.
 * Memory breakpoints stop right AFTER the instruction that made the access
 * (the access itself has already happened). */

#ifndef __THEODORE_DBG_H
#define __THEODORE_DBG_H

#include "theodore_api.h"

#include <stdint.h>
#include <stddef.h>

typedef struct
{
  uint8_t a, b, dp, cc;
  uint16_t x, y, u, s, pc;
} TheodoreRegs;

typedef enum { TDBG_BP_PC, TDBG_BP_READ, TDBG_BP_WRITE } TheodoreBpType;

typedef struct
{
  TheodoreBpType type;
  uint16_t addr;
} TheodoreBreakpoint;

typedef enum
{
  TDBG_STOP_NONE,
  TDBG_STOP_USER,
  TDBG_STOP_STEP,
  TDBG_STOP_PC,
  TDBG_STOP_READ,
  TDBG_STOP_WRITE,
  TDBG_STOP_DATA
} TheodoreStopReason;

typedef struct
{
  TheodoreStopReason reason;
  uint16_t addr;   /* breakpoint address, or PC for USER/STEP */
  uint8_t value;   /* data byte of the access (READ/WRITE/DATA stops) */
  uint8_t is_read; /* 1 = the access was a read, 0 = a write (DATA stops) */
  int data_id;     /* id of the data breakpoint (DATA stops), else -1 */
} TheodoreStopInfo;

/* Data-pattern breakpoints: stop right after an access whose data bytes match
   a pattern. Access kinds (bit mask): */
#define TDBG_ACCESS_READ  1
#define TDBG_ACCESS_WRITE 2
#define TDBG_DATA_BP_MAX  16
#define TDBG_DATA_LEN_MAX 4

typedef struct
{
  int id;
  unsigned access;                 /* TDBG_ACCESS_* mask */
  uint16_t first, last;            /* address range, inclusive */
  unsigned len;                    /* number of pattern bytes (1..4) */
  uint8_t value[TDBG_DATA_LEN_MAX];
  uint8_t mask[TDBG_DATA_LEN_MAX];
} TheodoreDataBreakpoint;

typedef struct
{
  uint64_t cycle;      /* cycle counter at the start of the instruction */
  TheodoreRegs regs;   /* registers BEFORE the instruction executes */
} TheodoreTraceEntry;

/* ---- Exported API (see link.T) ---- */
/* Returns 0 on success, -1 if the breakpoint table (64 entries) is full. */
THEODORE_API int  theodore_dbg_add_breakpoint(TheodoreBpType type, uint16_t addr);
THEODORE_API int  theodore_dbg_del_breakpoint(TheodoreBpType type, uint16_t addr);
THEODORE_API void theodore_dbg_clear_breakpoints(void);
/* Copies up to max breakpoints, returns how many exist. */
THEODORE_API unsigned theodore_dbg_list_breakpoints(TheodoreBreakpoint *out, unsigned max);

/* Data-pattern breakpoints. A breakpoint watches the accesses of the kinds in
   `access` whose address lies in [first, last]. It keeps the last `len`
   data bytes seen by those accesses (in access order, whatever their address
   inside the range) and stops when they equal `value[]` under `mask[]`:
   (byte & mask[i]) == (value[i] & mask[i]), the newest byte compared with
   value[len-1]. With len == 1 this is a plain "data matches pattern" test;
   with len > 1 it detects a byte sequence, e.g. a command written to a port.
   `mask` may be NULL (= 0xFF for every byte). Instruction fetches are reads
   and are seen too. The stop happens after the instruction that completed the
   pattern. Returns the breakpoint id (>= 0) or -1 (bad arguments or full). */
THEODORE_API int  theodore_dbg_add_data_breakpoint(unsigned access, uint16_t first, uint16_t last,
                                      const uint8_t *value, const uint8_t *mask, unsigned len);
THEODORE_API int  theodore_dbg_del_data_breakpoint(int id);
/* Copies up to max entries, returns how many exist. */
THEODORE_API unsigned theodore_dbg_list_data_breakpoints(TheodoreDataBreakpoint *out, unsigned max);

/* Stops emulation (takes effect at the next instruction boundary). */
THEODORE_API void theodore_dbg_halt(void);
/* Leaves the halted state. */
THEODORE_API void theodore_dbg_resume(void);
/* While halted, executes exactly one instruction and stays halted.
   Returns 0, or -1 if the emulator was not halted. */
THEODORE_API int  theodore_dbg_step(void);
THEODORE_API int  theodore_dbg_is_halted(void);
/* Number of times the emulation has stopped since the core started (user halt,
   step, any breakpoint). A frontend compares it with the last value it
   reported: unlike "was running, is halted now" it cannot miss a resume followed
   by a new stop within the same frame. */
THEODORE_API unsigned theodore_dbg_stop_count(void);
THEODORE_API TheodoreStopInfo theodore_dbg_stop_info(void);

THEODORE_API void theodore_dbg_get_regs(TheodoreRegs *r);
THEODORE_API void theodore_dbg_set_regs(const TheodoreRegs *r);

/* Side-effect-free memory access: no breakpoints, no peripheral callbacks,
   no bank switching. I/O addresses (A7C0-A7FF) return the raw port shadow.
   poke() only writes RAM (0000-9FFF); returns 0, or -1 if not writable.
   Both are implemented for MO5/MO6/PC128 only (other models: 0xFF / -1). */
THEODORE_API uint8_t theodore_dbg_peek(uint16_t addr);
THEODORE_API int     theodore_dbg_poke(uint16_t addr, uint8_t value);
/* Disassembles one instruction at addr into buf (with its hex bytes).
   Returns the address of the next instruction. */
THEODORE_API unsigned theodore_dbg_disasm(uint16_t addr, char *buf, size_t size);

/* Execution trace ring buffer (8192 entries, oldest entries are dropped). */
THEODORE_API void     theodore_dbg_trace_enable(int enable);
THEODORE_API void     theodore_dbg_trace_clear(void);
/* Copies up to max entries, oldest first. Returns the number copied. */
THEODORE_API unsigned theodore_dbg_trace_read(TheodoreTraceEntry *out, unsigned max);

/* ---- Internal hooks used by the emulator core ---- */
extern int tdbg_active;   /* nonzero when breakpoints or trace are enabled */
extern int tdbg_halted;   /* nonzero while stopped */
extern int tdbg_suppress; /* nonzero to bypass breakpoints and peripherals */
/* Returns nonzero if execution must stop before this instruction. */
int  tdbg_before_instruction(unsigned short pc);
/* Returns nonzero if a memory breakpoint requests a stop. */
int  tdbg_after_instruction(void);
/* `value` is the data byte actually read or written. */
void tdbg_mem_access(unsigned short addr, int is_read, unsigned char value);

#endif /* __THEODORE_DBG_H */
