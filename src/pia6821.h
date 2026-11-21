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

/* Motorola 6821 PIA model, attachable to the peripheral bus (periph.h).
 *
 * Modeled: data-direction and output registers, control registers, C1/C2
 * edge detection and IRQ flags (flags cleared by reading the data register),
 * C2 as input, as manual output, as read/write handshake and as one-cycle
 * pulse, and the two IRQ outputs.
 * Approximations (no hardware verification available here):
 *  - the one-cycle C2 pulse is reported as a low then a high event with the
 *    same cycle stamp, so no pulse width is modeled;
 *  - there is no contention model: an output bit reads back its output
 *    register, an input bit reads the pin level;
 *  - input pins and C1/C2 lines start high (idle, pulled up);
 *  - a reset clears every register.
 *
 * Register layout is selectable. Thomson machines wire the chip with the
 * select lines swapped relative to the data sheet's order: the emulator's own
 * stub for the music/game PIA decodes A7CC = port A, A7CD = port B,
 * A7CE = control A, A7CF = control B (THPIA_LAYOUT_THOMSON, the default). */

#ifndef __PIA6821_H
#define __PIA6821_H

#include "theodore_api.h"

#include <stdint.h>

typedef enum { THPIA_LAYOUT_THOMSON = 0, THPIA_LAYOUT_STANDARD = 1 } ThPiaLayout;
/* base+0..3 :  THOMSON  = port A, port B, control A, control B
                STANDARD = port A, control A, port B, control B */

typedef enum { THPIA_PORT_A = 0, THPIA_PORT_B = 1 } ThPiaPort;
typedef enum { THPIA_CA1 = 0, THPIA_CA2 = 1, THPIA_CB1 = 2, THPIA_CB2 = 3 } ThPiaLine;

typedef struct
{
  uint8_t ora, ddra, cra;
  uint8_t orb, ddrb, crb;
  uint8_t pins_a, pins_b;       /* external input pins as last set by the host */
  uint8_t ca1, ca2, cb1, cb2;   /* line levels at the pins (CA2/CB2: output level when an output) */
  uint8_t irqa, irqb;           /* IRQ outputs (1 = asserted) */
} ThPiaState;

typedef struct
{
  ThPiaLayout layout;
  /* The value driven on a port changed: bits with ddr = 1 are outputs and
     carry `out`; the others are inputs. Called after a write to the output
     or direction register. Optional. */
  void (*on_port_output)(void *ctx, ThPiaPort port, uint8_t out, uint8_t ddr, uint64_t cycle);
  /* CA2 or CB2 changed level while configured as an output. Optional. */
  void (*on_control_output)(void *ctx, ThPiaLine line, int level, uint64_t cycle);
  /* An IRQ output changed. Optional. */
  void (*on_irq)(void *ctx, int irqa, int irqb, uint64_t cycle);
  /* Optional on-demand input pins, called at each read of a data register.
     Return 0..255, or -1 to use the pins last set with theodore_pia_set_pins().
     Lets a lock-step bridge ask the real device at the moment of the access. */
  int (*read_pins)(void *ctx, ThPiaPort port, uint64_t cycle);
  void *ctx;
  /* Peripheral-bus IRQ source (1..31) driven by IRQA | IRQB. 0 or negative =
     not wired to the CPU (so a zero-initialized config is safe). Source 0 is
     reserved to theodore_periph_set_irq(). */
  int irq_source;
  /* Optional: called after every executed instruction (polling, deferred work). */
  void (*tick)(void *ctx, uint64_t cycle);
} ThPiaConfig;

/* ---- Exported API (see link.T) ---- */
/* Registers a PIA on the 4 addresses base..base+3. Returns its id (>= 0) or
   -1 on error (bad arguments, no free PIA or peripheral entry). */
THEODORE_API int  theodore_pia_attach(unsigned short base, const ThPiaConfig *cfg);
/* Removes the PIA and releases its IRQ source. Returns 0 or -1. */
THEODORE_API int  theodore_pia_detach(int id);
/* Sets the external level of the 8 input pins of a port. */
THEODORE_API void theodore_pia_set_pins(int id, ThPiaPort port, uint8_t value);
/* Sets the level of a control line; edges raise the IRQ flags. */
THEODORE_API void theodore_pia_set_line(int id, ThPiaLine line, int level);
THEODORE_API int  theodore_pia_get_state(int id, ThPiaState *st);
/* Clears every register (like a hardware reset). */
THEODORE_API void theodore_pia_reset(int id);
/* Resets every attached PIA (called on machine reset). */
THEODORE_API void theodore_pia_reset_all(void);

#endif /* __PIA6821_H */
