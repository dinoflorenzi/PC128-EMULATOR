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

/* Attachment points for external hardware on MO5/MO6/PC128: the expansion
 * bus, the cartridge connector and the joystick ports (music/game 6821 PIA).
 * Everything here is built on the peripheral bus (periph.h): a slot only
 * decides which addresses a device may claim and checks them.
 *
 * Address facts below come from the emulator's own memory map (MgetMo/MputMo),
 * not from hardware documentation:
 *  - cartridge memory space ........ B000-EFFF
 *  - cartridge control (carflags) .. A7CB (the core ties it to cartridges)
 *  - music/game PIA (joysticks) .... A7CC-A7CF
 *  - addresses the core already handles specially in the I/O area:
 *    A000-A7BF (CD90-640 controller ROM), A7C0-A7C3 (system PIA), A7CB-A7CF,
 *    A7D8, A7DA-A7DD, A7E1, A7E4-A7E7.
 *  Other addresses of A7C0-A7FF only hit a plain register shadow in the core
 *  and are free for an expansion device. */

#ifndef __SLOTS_H
#define __SLOTS_H

#include "theodore_api.h"

#include "periph.h"
#include "pia6821.h"

typedef enum
{
  THSLOT_EXPANSION = 0,
  THSLOT_CARTRIDGE = 1,
  THSLOT_JOYSTICK  = 2
} ThSlot;

typedef struct
{
  /* EXPANSION: window start and size, mandatory, inside A000-A7FF.
     CARTRIDGE: optional sub-window of B000-EFFF; size 0 = the whole space.
     JOYSTICK : ignored (the PIA sits at A7CC-A7CF). */
  unsigned short base, size;
  /* Device callbacks for EXPANSION and CARTRIDGE (read, write, tick, ctx,
     name); base and size of this structure are ignored. */
  TheodorePeripheral dev;
  /* EXPANSION: accept addresses the core already handles natively (the device
     then shadows the built-in behavior). */
  int override_native;
  /* CARTRIDGE: also claim A7CB with the same callbacks. The built-in cartridge
     banking is then bypassed while the slot is attached. */
  int cartridge_control;
  /* Peripheral-bus IRQ source (1..31) wired to this slot; 0 or negative = not
     connected (the default of a zero-initialized config). For JOYSTICK it
     overrides pia.irq_source. How the PIA IRQ line reaches the CPU on real
     hardware is not documented in the core, so nothing is wired by default. */
  int irq_source;
  /* JOYSTICK: PIA callbacks and layout (layout defaults to THOMSON). */
  ThPiaConfig pia;
  /* JOYSTICK: 1 = the built-in joystick (pads, keyboard mapping of the
     frontend) keeps driving the input pins, wired-AND with the external pins
     (the lines idle high and a switch pulls them low), and writes to the 6-bit
     DAC on port B bits 0-5 (outputs per DDRB) still drive the speaker.
     0 = only external pins, no sound.
     Known difference with the core's native (PIA-less) joystick path: when
     DDRB bits 0-5 are inputs the PIA model reads them as 1 (idle pull-up),
     the native path returns the DAC level (0 after boot). With DDRB = 3F, as
     a program producing sound configures it, both return the DAC value. */
  int builtin_joystick;
} ThSlotConfig;

/* ---- Exported API (see link.T) ---- */
/* Attaches a device to a slot. Returns 0, or -1 with the reason available
   from theodore_slot_error(). A slot can hold one device: detach first. */
THEODORE_API int  theodore_slot_attach(ThSlot slot, const ThSlotConfig *cfg);
THEODORE_API void theodore_slot_detach(ThSlot slot);
THEODORE_API int  theodore_slot_is_attached(ThSlot slot);
/* Id of the PIA behind the joystick slot (for theodore_pia_*), or -1. */
THEODORE_API int  theodore_slot_pia(void);
THEODORE_API const char *theodore_slot_error(void);

#endif /* __SLOTS_H */
