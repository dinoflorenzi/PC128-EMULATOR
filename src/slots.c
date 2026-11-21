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

#include "slots.h"
#include "motoemulator.h"

#include <stdio.h>
#include <string.h>

#define SLOT_COUNT 3
#define JOY_BASE 0xA7CC

typedef struct
{
  int attached;
  int n;                       /* windows in use (1 or 2) */
  int periph_id[2];
  unsigned short base[2], size[2];
} SlotState;

static SlotState slots[SLOT_COUNT];
static char errbuf[200] = "";

/* Joystick slot state */
static int joy_pia = -1;
static ThPiaConfig joy_user;
static int joy_builtin = 0;

static int fail(const char *fmt, unsigned a, unsigned b)
{
  snprintf(errbuf, sizeof(errbuf), fmt, a, b);
  return -1;
}

const char *theodore_slot_error(void)
{
  return errbuf;
}

/* Addresses MgetMo/MputMo treat specially (see slots.h). */
static int native_addr(unsigned a)
{
  if (a >= 0xA000 && a <= 0xA7BF) return 1;
  if (a >= 0xA7C0 && a <= 0xA7C3) return 1;
  if (a >= 0xA7CB && a <= 0xA7CF) return 1;
  if (a == 0xA7D8) return 1;
  if (a >= 0xA7DA && a <= 0xA7DD) return 1;
  if (a == 0xA7E1) return 1;
  if (a >= 0xA7E4 && a <= 0xA7E7) return 1;
  return 0;
}

static int overlaps_other_slot(ThSlot self, unsigned first, unsigned last)
{
  int s, i;
  for (s = 0; s < SLOT_COUNT; s++)
  {
    if (s == (int) self || !slots[s].attached) continue;
    for (i = 0; i < slots[s].n; i++)
    {
      unsigned f = slots[s].base[i], l = f + slots[s].size[i] - 1u;
      if (first <= l && f <= last) return (int) f;
    }
  }
  return -1;
}

int theodore_slot_is_attached(ThSlot slot)
{
  return (int) slot >= 0 && (int) slot < SLOT_COUNT && slots[slot].attached;
}

int theodore_slot_pia(void)
{
  return slots[THSLOT_JOYSTICK].attached ? joy_pia : -1;
}

/* ---- joystick slot glue ---- */
static int joy_read_pins(void *ctx, ThPiaPort port, uint64_t cycle)
{
  int v = -1;
  (void) ctx;
  if (joy_user.read_pins != NULL) v = joy_user.read_pins(joy_user.ctx, port, cycle);
  if (v < 0)
  {
    ThPiaState st;
    theodore_pia_get_state(joy_pia, &st);
    v = port == THPIA_PORT_A ? st.pins_a : st.pins_b;
  }
  if (joy_builtin)
  {
    uint8_t pa, pb;
    toemulator_get_joystick_pins(&pa, &pb);
    v &= port == THPIA_PORT_A ? pa : pb;
  }
  return v;
}

static void joy_port_output(void *ctx, ThPiaPort port, uint8_t out, uint8_t ddr, uint64_t cycle)
{
  if (joy_builtin && port == THPIA_PORT_B) toemulator_set_sound(out & ddr);
  if (joy_user.on_port_output != NULL) joy_user.on_port_output(joy_user.ctx, port, out, ddr, cycle);
  (void) ctx;
}

static void joy_control_output(void *ctx, ThPiaLine line, int level, uint64_t cycle)
{
  (void) ctx;
  if (joy_user.on_control_output != NULL) joy_user.on_control_output(joy_user.ctx, line, level, cycle);
}

static void joy_irq(void *ctx, int irqa, int irqb, uint64_t cycle)
{
  (void) ctx;
  if (joy_user.on_irq != NULL) joy_user.on_irq(joy_user.ctx, irqa, irqb, cycle);
}

static void joy_tick(void *ctx, uint64_t cycle)
{
  (void) ctx;
  if (joy_user.tick != NULL) joy_user.tick(joy_user.ctx, cycle);
}

static int attach_joystick(const ThSlotConfig *cfg)
{
  ThPiaConfig pc;
  int r = overlaps_other_slot(THSLOT_JOYSTICK, JOY_BASE, JOY_BASE + 3);
  if (r >= 0) return fail("joystick PIA A7CC-A7CF overlaps another slot at %04X%.0u", (unsigned) r, 0);
  joy_user = cfg->pia;
  joy_builtin = cfg->builtin_joystick != 0;
  pc = cfg->pia;
  pc.layout = cfg->pia.layout; /* THPIA_LAYOUT_THOMSON (0) by default */
  pc.ctx = NULL;
  pc.read_pins = joy_read_pins;
  pc.on_port_output = joy_port_output;
  pc.on_control_output = joy_control_output;
  pc.on_irq = joy_irq;
  pc.tick = cfg->pia.tick != NULL ? joy_tick : NULL;
  pc.irq_source = cfg->irq_source > 0 ? cfg->irq_source : (cfg->pia.irq_source > 0 ? cfg->pia.irq_source : 0);
  joy_pia = theodore_pia_attach(JOY_BASE, &pc);
  if (joy_pia < 0) return fail("cannot register the PIA (no free PIA or peripheral entry)%.0u%.0u", 0, 0);
  slots[THSLOT_JOYSTICK].n = 1;
  slots[THSLOT_JOYSTICK].base[0] = JOY_BASE;
  slots[THSLOT_JOYSTICK].size[0] = 4;
  slots[THSLOT_JOYSTICK].periph_id[0] = -1; /* owned by the PIA module */
  return 0;
}

int theodore_slot_attach(ThSlot slot, const ThSlotConfig *cfg)
{
  unsigned base, size, last, a;
  TheodorePeripheral per;
  int n, i, r;
  errbuf[0] = '\0';
  if (cfg == NULL || (int) slot < 0 || (int) slot >= SLOT_COUNT) return fail("invalid slot or configuration%.0u%.0u", 0, 0);
  if (slots[slot].attached) return fail("slot already in use, detach it first%.0u%.0u", 0, 0);
  if (cfg->irq_source > 31 || cfg->pia.irq_source > 31) return fail("IRQ source %u out of range (1..31)%.0u", (unsigned) cfg->irq_source, 0);

  if (slot == THSLOT_JOYSTICK)
  {
    if (attach_joystick(cfg) != 0) return -1;
    slots[slot].attached = 1;
    return 0;
  }

  if (cfg->dev.read == NULL || cfg->dev.write == NULL)
  {
    return fail("device read and write callbacks are mandatory%.0u%.0u", 0, 0);
  }

  if (slot == THSLOT_EXPANSION)
  {
    base = cfg->base;
    size = cfg->size;
    if (size == 0) return fail("expansion window size is mandatory%.0u%.0u", 0, 0);
    last = base + size - 1u;
    if (base < 0xA000 || last > 0xA7FF)
    {
      return fail("expansion window %04X-%04X is not inside A000-A7FF", base, last);
    }
    if (!cfg->override_native)
    {
      for (a = base; a <= last; a++)
      {
        if (native_addr(a))
        {
          return fail("address %04X is handled natively by the core (use override_native to shadow it)%.0u", a, 0);
        }
      }
    }
    r = overlaps_other_slot(slot, base, last);
    if (r >= 0) return fail("window overlaps another slot at %04X%.0u", (unsigned) r, 0);
    n = 1;
  }
  else /* THSLOT_CARTRIDGE */
  {
    base = cfg->size == 0 ? 0xB000u : cfg->base;
    size = cfg->size == 0 ? 0x4000u : cfg->size;
    last = base + size - 1u;
    if (base < 0xB000 || last > 0xEFFF)
    {
      return fail("cartridge window %04X-%04X is not inside B000-EFFF", base, last);
    }
    r = overlaps_other_slot(slot, base, last);
    if (r >= 0) return fail("window overlaps another slot at %04X%.0u", (unsigned) r, 0);
    n = cfg->cartridge_control ? 2 : 1;
    if (n == 2 && overlaps_other_slot(slot, 0xA7CB, 0xA7CB) >= 0)
    {
      return fail("A7CB is already claimed by another slot%.0u%.0u", 0, 0);
    }
  }

  per = cfg->dev;
  slots[slot].n = 0;
  for (i = 0; i < n; i++)
  {
    per.base = i == 0 ? (unsigned short) base : (unsigned short) 0xA7CB;
    per.size = i == 0 ? (unsigned short) size : (unsigned short) 1;
    slots[slot].periph_id[i] = theodore_periph_register(&per);
    if (slots[slot].periph_id[i] < 0)
    {
      while (--i >= 0) theodore_periph_unregister(slots[slot].periph_id[i]);
      return fail("peripheral table is full%.0u%.0u", 0, 0);
    }
    slots[slot].base[i] = per.base;
    slots[slot].size[i] = per.size;
    slots[slot].n = i + 1;
  }
  slots[slot].attached = 1;
  return 0;
}

void theodore_slot_detach(ThSlot slot)
{
  int i;
  if ((int) slot < 0 || (int) slot >= SLOT_COUNT || !slots[slot].attached) return;
  if (slot == THSLOT_JOYSTICK)
  {
    theodore_pia_detach(joy_pia);
    joy_pia = -1;
    toemulator_set_sound(0);
  }
  else
  {
    for (i = 0; i < slots[slot].n; i++) theodore_periph_unregister(slots[slot].periph_id[i]);
  }
  memset(&slots[slot], 0, sizeof(slots[slot]));
}
