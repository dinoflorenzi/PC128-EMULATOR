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

#include "theodore_dbg.h"
#include "periph.h"
#include "6809cpu.h"
#include "6809disasm.h"
#include "motoemulator.h"

#include <stdio.h>
#include <string.h>

#define BP_MAX 64
#define TRACE_SIZE 8192

int tdbg_active = 0;
int tdbg_halted = 0;
int tdbg_suppress = 0;

static TheodoreBreakpoint bps[BP_MAX];
static int bp_total = 0;
static int bp_count[3] = { 0, 0, 0 }; /* per TheodoreBpType */
static int trace_on = 0;
static TheodoreTraceEntry ring[TRACE_SIZE];
static unsigned ring_head = 0;  /* next slot to write */
static unsigned ring_count = 0;
static int skip_once = 0;       /* do not re-trigger a PC breakpoint right after resume/step */
static int pending_stop = 0;
static unsigned stop_counter = 0;
static TheodoreStopInfo stop = { TDBG_STOP_NONE, 0, 0, 0, -1 };

typedef struct
{
  int used;
  TheodoreDataBreakpoint bp;
  uint8_t hist[TDBG_DATA_LEN_MAX]; /* last bytes seen, hist[0] oldest of the kept ones */
  unsigned seen;                   /* how many bytes were seen so far (saturating) */
} DataBp;
static DataBp dbps[TDBG_DATA_BP_MAX];
static int dbp_count = 0;

static void update_active(void)
{
  tdbg_active = (bp_total > 0) || (dbp_count > 0) || trace_on;
}

static int has_bp(TheodoreBpType type, uint16_t addr)
{
  int i;
  if (bp_count[type] == 0) return 0;
  for (i = 0; i < bp_total; i++)
  {
    if (bps[i].type == type && bps[i].addr == addr) return 1;
  }
  return 0;
}

int theodore_dbg_add_breakpoint(TheodoreBpType type, uint16_t addr)
{
  if ((int) type < 0 || (int) type > 2) return -1;
  if (has_bp(type, addr)) return 0;
  if (bp_total >= BP_MAX) return -1;
  bps[bp_total].type = type;
  bps[bp_total].addr = addr;
  bp_total++;
  bp_count[type]++;
  update_active();
  return 0;
}

int theodore_dbg_del_breakpoint(TheodoreBpType type, uint16_t addr)
{
  int i;
  for (i = 0; i < bp_total; i++)
  {
    if (bps[i].type == type && bps[i].addr == addr)
    {
      bps[i] = bps[bp_total - 1];
      bp_total--;
      bp_count[type]--;
      update_active();
      return 0;
    }
  }
  return -1;
}

void theodore_dbg_clear_breakpoints(void)
{
  bp_total = 0;
  memset(bp_count, 0, sizeof(bp_count));
  memset(dbps, 0, sizeof(dbps));
  dbp_count = 0;
  update_active();
}

unsigned theodore_dbg_list_breakpoints(TheodoreBreakpoint *out, unsigned max)
{
  unsigned i;
  for (i = 0; out != NULL && i < max && i < (unsigned) bp_total; i++) out[i] = bps[i];
  return (unsigned) bp_total;
}

int theodore_dbg_add_data_breakpoint(unsigned access, uint16_t first, uint16_t last,
                                     const uint8_t *value, const uint8_t *mask, unsigned len)
{
  int id;
  unsigned i;
  if ((access & ~(unsigned) (TDBG_ACCESS_READ | TDBG_ACCESS_WRITE)) != 0 || access == 0
      || first > last || value == NULL || len < 1 || len > TDBG_DATA_LEN_MAX)
  {
    return -1;
  }
  for (id = 0; id < TDBG_DATA_BP_MAX && dbps[id].used; id++) {}
  if (id >= TDBG_DATA_BP_MAX) return -1;
  memset(&dbps[id], 0, sizeof(dbps[id]));
  dbps[id].used = 1;
  dbps[id].bp.id = id;
  dbps[id].bp.access = access;
  dbps[id].bp.first = first;
  dbps[id].bp.last = last;
  dbps[id].bp.len = len;
  for (i = 0; i < len; i++)
  {
    dbps[id].bp.mask[i] = mask != NULL ? mask[i] : 0xFF;
    dbps[id].bp.value[i] = (uint8_t) (value[i] & dbps[id].bp.mask[i]);
  }
  dbp_count++;
  update_active();
  return id;
}

int theodore_dbg_del_data_breakpoint(int id)
{
  if (id < 0 || id >= TDBG_DATA_BP_MAX || !dbps[id].used) return -1;
  dbps[id].used = 0;
  dbp_count--;
  update_active();
  return 0;
}

unsigned theodore_dbg_list_data_breakpoints(TheodoreDataBreakpoint *out, unsigned max)
{
  unsigned n = 0;
  int i;
  for (i = 0; i < TDBG_DATA_BP_MAX; i++)
  {
    if (!dbps[i].used) continue;
    if (out != NULL && n < max) out[n] = dbps[i].bp;
    n++;
  }
  return n;
}

void theodore_dbg_get_regs(TheodoreRegs *r)
{
  r->a = (uint8_t) *dc6809_a;
  r->b = (uint8_t) *dc6809_b;
  r->dp = (uint8_t) *dc6809_dp;
  r->cc = (uint8_t) dc6809_cc;
  r->x = (uint16_t) dc6809_x;
  r->y = (uint16_t) dc6809_y;
  r->u = (uint16_t) dc6809_u;
  r->s = (uint16_t) dc6809_s;
  r->pc = dc6809_pc;
}

void theodore_dbg_set_regs(const TheodoreRegs *r)
{
  *dc6809_a = (char) r->a;
  *dc6809_b = (char) r->b;
  *dc6809_dp = (char) r->dp;
  dc6809_cc = (char) r->cc;
  dc6809_x = (short) r->x;
  dc6809_y = (short) r->y;
  dc6809_u = (short) r->u;
  dc6809_s = (short) r->s;
  dc6809_pc = r->pc;
}

void theodore_dbg_halt(void)
{
  if (!tdbg_halted)
  {
    tdbg_halted = 1;
    stop_counter++;
    stop.reason = TDBG_STOP_USER;
    stop.addr = dc6809_pc;
    stop.data_id = -1;
  }
}

void theodore_dbg_resume(void)
{
  tdbg_halted = 0;
  pending_stop = 0;
  skip_once = 1;
  stop.reason = TDBG_STOP_NONE;
}

int theodore_dbg_step(void)
{
  if (!tdbg_halted) return -1;
  tdbg_halted = 0;
  pending_stop = 0;
  skip_once = 1;
  stop.reason = TDBG_STOP_STEP;
  stop.addr = dc6809_pc;
  stop.data_id = -1;
  Run(1); /* executes exactly one instruction: ncycles >= 1 ends the loop */
  tdbg_halted = 1;
  stop_counter++;
  return 0;
}

unsigned theodore_dbg_stop_count(void)
{
  return stop_counter;
}

int theodore_dbg_is_halted(void)
{
  return tdbg_halted;
}

TheodoreStopInfo theodore_dbg_stop_info(void)
{
  return stop;
}

uint8_t theodore_dbg_peek(uint16_t addr)
{
  return toemulator_peek(addr);
}

int theodore_dbg_poke(uint16_t addr, uint8_t value)
{
  return toemulator_poke(addr, value);
}

static char peek_char(unsigned short a)
{
  return (char) toemulator_peek(a);
}

unsigned theodore_dbg_disasm(uint16_t addr, char *buf, size_t size)
{
  char tmp[256];
  char (*saved)(unsigned short) = Mgetc;
  int next;
  /* The disassembler reads through Mgetc: swap in the side-effect-free reader. */
  Mgetc = peek_char;
  tmp[0] = '\0';
  next = dasm6809(addr, tmp);
  Mgetc = saved;
  if (buf != NULL && size > 0)
  {
    snprintf(buf, size, "%s", tmp);
  }
  return (unsigned) next & 0xFFFF;
}

void theodore_dbg_trace_enable(int enable)
{
  trace_on = enable != 0;
  update_active();
}

void theodore_dbg_trace_clear(void)
{
  ring_head = 0;
  ring_count = 0;
}

unsigned theodore_dbg_trace_read(TheodoreTraceEntry *out, unsigned max)
{
  unsigned i, n = ring_count < max ? ring_count : max;
  unsigned first = (ring_head + TRACE_SIZE - ring_count) % TRACE_SIZE;
  /* if the caller's buffer is smaller than the ring, return the newest entries */
  first = (first + (ring_count - n)) % TRACE_SIZE;
  for (i = 0; out != NULL && i < n; i++) out[i] = ring[(first + i) % TRACE_SIZE];
  return n;
}

/* ---- hooks called by the emulator core ---- */

int tdbg_before_instruction(unsigned short pc)
{
  if (skip_once)
  {
    skip_once = 0;
  }
  else if (has_bp(TDBG_BP_PC, pc))
  {
    stop.reason = TDBG_STOP_PC;
    stop.addr = pc;
    stop.data_id = -1;
    tdbg_halted = 1;
    stop_counter++;
    return 1;
  }
  if (trace_on)
  {
    TheodoreTraceEntry *e = &ring[ring_head];
    e->cycle = theodore_periph_cycle();
    theodore_dbg_get_regs(&e->regs);
    ring_head = (ring_head + 1) % TRACE_SIZE;
    if (ring_count < TRACE_SIZE) ring_count++;
  }
  return 0;
}

int tdbg_after_instruction(void)
{
  if (pending_stop)
  {
    pending_stop = 0;
    tdbg_halted = 1;
    stop_counter++;
    return 1;
  }
  return 0;
}

void tdbg_mem_access(unsigned short addr, int is_read, unsigned char value)
{
  TheodoreBpType t = is_read ? TDBG_BP_READ : TDBG_BP_WRITE;
  unsigned kind = is_read ? TDBG_ACCESS_READ : TDBG_ACCESS_WRITE;
  int i;
  if (tdbg_suppress) return;
  /* Data breakpoints must see every access to keep their byte history right,
     even when an earlier hit of the same instruction already requested a stop. */
  for (i = 0; dbp_count > 0 && i < TDBG_DATA_BP_MAX; i++)
  {
    DataBp *d = &dbps[i];
    unsigned k, ok;
    if (!d->used || !(d->bp.access & kind) || addr < d->bp.first || addr > d->bp.last) continue;
    for (k = 0; k + 1 < TDBG_DATA_LEN_MAX; k++) d->hist[k] = d->hist[k + 1];
    d->hist[TDBG_DATA_LEN_MAX - 1] = value;
    if (d->seen < TDBG_DATA_LEN_MAX) d->seen++;
    if (d->seen < d->bp.len) continue;
    ok = 1;
    for (k = 0; k < d->bp.len; k++)
    {
      uint8_t b = d->hist[TDBG_DATA_LEN_MAX - d->bp.len + k];
      if ((b & d->bp.mask[k]) != d->bp.value[k]) { ok = 0; break; }
    }
    if (ok && !pending_stop)
    {
      pending_stop = 1;
      stop.reason = TDBG_STOP_DATA;
      stop.addr = addr;
      stop.value = value;
      stop.is_read = (uint8_t) is_read;
      stop.data_id = d->bp.id;
    }
  }
  if (pending_stop) return;
  if (has_bp(t, addr))
  {
    pending_stop = 1;
    stop.reason = is_read ? TDBG_STOP_READ : TDBG_STOP_WRITE;
    stop.addr = addr;
    stop.value = value;
    stop.is_read = (uint8_t) is_read;
    stop.data_id = -1;
  }
}
