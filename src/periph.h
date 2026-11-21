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

/* External peripheral bus (MO5/MO6/PC128 only).
 *
 * A peripheral claims a range of 6809 addresses. Every CPU read/write that
 * falls in the range is forwarded to the peripheral's callbacks, which can
 * be implemented in C inside the host, replay a script, or forward the access
 * to real hardware (e.g. a microcontroller on a serial/USB/TCP link).
 *
 * Cycle stamps: the `cycle` argument is the number of 6809 cycles elapsed
 * since core start (or last reset) at the START of the instruction that
 * performs the access. All accesses made by one instruction share the same
 * stamp. */

#ifndef __PERIPH_H
#define __PERIPH_H

#include "theodore_api.h"

#include <stdint.h>
#include "boolean.h"

#define THEODORE_PERIPH_MAX 16
/* Value returned by a read callback to let the emulator handle the access. */
#define THEODORE_PERIPH_PASS (-1)

typedef struct
{
  const char *name;           /* for diagnostics, may be NULL */
  unsigned short base;        /* first address handled */
  unsigned short size;        /* number of consecutive addresses (> 0) */
  /* Returns the byte read (0..255) or THEODORE_PERIPH_PASS. Mandatory. */
  int (*read)(void *ctx, unsigned short addr, uint64_t cycle);
  /* Returns nonzero if the write was consumed, 0 to let the emulator also
     handle it. Mandatory. */
  int (*write)(void *ctx, unsigned short addr, unsigned char value, uint64_t cycle);
  /* Optional: called after every instruction with the new cycle count. */
  void (*tick)(void *ctx, uint64_t cycle);
  void *ctx;
  /* Optional: called on a machine reset (the cycle counter restarts at 0). */
  void (*reset)(void *ctx);
} TheodorePeripheral;

/* Exported API (see link.T). */
/* Registers a peripheral. Returns its id (>= 0) or -1 if the table is full
   or the descriptor is invalid. The descriptor is copied. */
THEODORE_API int theodore_periph_register(const TheodorePeripheral *p);
/* Removes one peripheral (ids of the others stay valid). Returns 0 or -1. */
THEODORE_API int theodore_periph_unregister(int id);
/* Removes all peripherals and releases every IRQ source. */
THEODORE_API void theodore_periph_clear(void);
/* The CPU IRQ line is the logical OR of up to 32 sources (0..31), so several
   peripherals can share it. While at least one source is asserted the 6809
   takes the interrupt whenever its I flag is clear (level-sensitive).
   theodore_periph_set_irq() drives source 0. */
THEODORE_API void theodore_periph_set_irq_source(int source, int level);
THEODORE_API void theodore_periph_set_irq(int level);
/* Bit mask of the sources currently asserted (diagnostics). */
THEODORE_API uint32_t theodore_periph_irq_mask(void);
/* Cycles elapsed since start/reset. */
THEODORE_API uint64_t theodore_periph_cycle(void);

/* Internal hooks used by the emulator core. */
extern uint8_t periph_pages[256]; /* nonzero = page 0xNN00 has a peripheral */
bool periph_read(unsigned short a, unsigned char *out);
bool periph_write(unsigned short a, unsigned char value);
/* Called by Run() after each executed instruction. */
void periph_after_instruction(int opcycles);
/* Called on machine reset: restarts the cycle counter and resets the 6821 PIAs
   and every peripheral that has a reset callback. */
void periph_reset_cycle(void);

#endif /* __PERIPH_H */
