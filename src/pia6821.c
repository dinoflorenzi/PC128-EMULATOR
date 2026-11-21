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

#include "pia6821.h"
#include "periph.h"

#include <string.h>

#define PIA_MAX 8

/* Control register bits (same meaning for CRA and CRB). */
#define CR_IRQ1_EN   0x01  /* C1 interrupt enable */
#define CR_C1_RISING 0x02  /* C1 active edge: 1 = low-to-high */
#define CR_DATA_SEL  0x04  /* 1 = data register, 0 = direction register */
#define CR_C2_BIT3   0x08  /* input: IRQ enable / output: manual level or pulse select */
#define CR_C2_BIT4   0x10  /* input: edge (1 = rising) / output: 1 = manual, 0 = handshake */
#define CR_C2_OUT    0x20  /* 1 = C2 is an output */
#define CR_IRQ2_FLAG 0x40
#define CR_IRQ1_FLAG 0x80

/* One side (A or B) of the chip. */
typedef struct
{
  uint8_t or_, ddr, cr;
  uint8_t pins;   /* external input pins */
  uint8_t c1;     /* C1 level */
  uint8_t c2_in;  /* external C2 level (used when C2 is an input) */
  uint8_t c2_hs;  /* handshake output level (1 = idle high) */
  uint8_t c2_pin; /* level currently at the C2 pin */
  uint8_t irq;
} Side;

typedef struct
{
  int used;
  int periph_id;
  ThPiaConfig cfg;
  Side s[2];
} Pia;

static Pia pias[PIA_MAX];

static void side_reset(Side *s)
{
  memset(s, 0, sizeof(*s));
  s->pins = 0xFF;
  s->c1 = 1;
  s->c2_in = 1;
  s->c2_hs = 1;
  s->c2_pin = 1;
}

static int c2_is_output(const Side *s) { return (s->cr & CR_C2_OUT) != 0; }

/* Level the C2 pin should show given the current mode. */
static int c2_level(const Side *s)
{
  if (!c2_is_output(s)) return s->c2_in;
  if (s->cr & CR_C2_BIT4) return (s->cr & CR_C2_BIT3) ? 1 : 0; /* manual */
  return s->c2_hs;                                              /* handshake / pulse */
}

static void notify_irq(Pia *p, int before_a, int before_b, uint64_t cycle)
{
  if (p->s[0].irq == before_a && p->s[1].irq == before_b) return;
  if (p->cfg.irq_source > 0)
  {
    theodore_periph_set_irq_source(p->cfg.irq_source, p->s[0].irq || p->s[1].irq);
  }
  if (p->cfg.on_irq != NULL) p->cfg.on_irq(p->cfg.ctx, p->s[0].irq, p->s[1].irq, cycle);
}

static void recompute_irq(Pia *p, uint64_t cycle)
{
  int before_a = p->s[0].irq, before_b = p->s[1].irq, i;
  for (i = 0; i < 2; i++)
  {
    const Side *s = &p->s[i];
    int irq1 = (s->cr & CR_IRQ1_FLAG) && (s->cr & CR_IRQ1_EN);
    int irq2 = (s->cr & CR_IRQ2_FLAG) && (s->cr & CR_C2_BIT3) && !c2_is_output(s);
    p->s[i].irq = (uint8_t) (irq1 || irq2);
  }
  notify_irq(p, before_a, before_b, cycle);
}

/* Updates the C2 pin from the mode; reports changes of an output level. */
static void refresh_c2(Pia *p, int side, uint64_t cycle)
{
  Side *s = &p->s[side];
  int lvl = c2_level(s);
  if (lvl == s->c2_pin) return;
  s->c2_pin = (uint8_t) lvl;
  if (c2_is_output(s) && p->cfg.on_control_output != NULL)
  {
    p->cfg.on_control_output(p->cfg.ctx, side == 0 ? THPIA_CA2 : THPIA_CB2, lvl, cycle);
  }
}

/* Handshake strobe: C2 goes low (read of port A / write of port B). */
static void strobe_c2(Pia *p, int side, uint64_t cycle)
{
  Side *s = &p->s[side];
  if (!c2_is_output(s) || (s->cr & CR_C2_BIT4)) return; /* not in handshake mode */
  s->c2_hs = 0;
  refresh_c2(p, side, cycle);
  if (s->cr & CR_C2_BIT3) /* pulse mode: back high right away (no width modeled) */
  {
    s->c2_hs = 1;
    refresh_c2(p, side, cycle);
  }
}

static void notify_port(Pia *p, int side, uint64_t cycle)
{
  if (p->cfg.on_port_output != NULL)
  {
    p->cfg.on_port_output(p->cfg.ctx, side == 0 ? THPIA_PORT_A : THPIA_PORT_B,
                          p->s[side].or_, p->s[side].ddr, cycle);
  }
}

/* Register offset (0..3) -> (side, is_control). */
static void decode(const Pia *p, unsigned off, int *side, int *is_ctrl)
{
  if (p->cfg.layout == THPIA_LAYOUT_STANDARD)
  {
    *side = (off >> 1) & 1;
    *is_ctrl = off & 1;
  }
  else
  {
    *side = off & 1;
    *is_ctrl = (off >> 1) & 1;
  }
}

static int pia_read_cb(void *ctx, unsigned short addr, uint64_t cycle)
{
  Pia *p = ctx;
  int side, is_ctrl;
  Side *s;
  /* attach() guarantees a 4-aligned base, so the register is addr modulo 4 */
  decode(p, (unsigned) addr & 3, &side, &is_ctrl);
  s = &p->s[side];
  if (is_ctrl) return s->cr;
  if (!(s->cr & CR_DATA_SEL)) return s->ddr;
  {
    int pins = -1, v;
    if (p->cfg.read_pins != NULL) pins = p->cfg.read_pins(p->cfg.ctx, side == 0 ? THPIA_PORT_A : THPIA_PORT_B, cycle);
    s->pins = pins >= 0 ? (uint8_t) pins : s->pins;
    v = (s->pins & ~s->ddr) | (s->or_ & s->ddr);
    /* reading the data register clears both interrupt flags of that side */
    if (s->cr & (CR_IRQ1_FLAG | CR_IRQ2_FLAG))
    {
      s->cr &= (uint8_t) ~(CR_IRQ1_FLAG | CR_IRQ2_FLAG);
      recompute_irq(p, cycle);
    }
    if (side == 0) strobe_c2(p, 0, cycle); /* read handshake exists on side A only */
    return v & 0xFF;
  }
}

static int pia_write_cb(void *ctx, unsigned short addr, unsigned char value, uint64_t cycle)
{
  Pia *p = ctx;
  int side, is_ctrl;
  Side *s;
  decode(p, (unsigned) addr & 3, &side, &is_ctrl);
  s = &p->s[side];
  if (is_ctrl)
  {
    s->cr = (uint8_t) ((s->cr & (CR_IRQ1_FLAG | CR_IRQ2_FLAG)) | (value & 0x3F));
    if (!c2_is_output(s) || (s->cr & CR_C2_BIT4)) s->c2_hs = 1; /* handshake idles high */
    refresh_c2(p, side, cycle);
    recompute_irq(p, cycle);
  }
  else if (s->cr & CR_DATA_SEL)
  {
    s->or_ = value;
    notify_port(p, side, cycle);
    if (side == 1) strobe_c2(p, 1, cycle); /* write handshake exists on side B only */
  }
  else
  {
    s->ddr = value;
    notify_port(p, side, cycle);
  }
  return 1;
}

static void pia_tick_cb(void *ctx, uint64_t cycle)
{
  Pia *p = ctx;
  if (p->cfg.tick != NULL) p->cfg.tick(p->cfg.ctx, cycle);
}

int theodore_pia_attach(unsigned short base, const ThPiaConfig *cfg)
{
  int id;
  TheodorePeripheral per;
  if (cfg == NULL || (base & 3) != 0 || (unsigned) base + 4 > 0x10000
      || cfg->irq_source > 31)
  {
    return -1;
  }
  for (id = 0; id < PIA_MAX && pias[id].used; id++) {}
  if (id >= PIA_MAX) return -1;
  memset(&pias[id], 0, sizeof(pias[id]));
  pias[id].cfg = *cfg;
  side_reset(&pias[id].s[0]);
  side_reset(&pias[id].s[1]);
  memset(&per, 0, sizeof(per));
  per.name = "pia6821";
  per.base = base;
  per.size = 4;
  per.read = pia_read_cb;
  per.write = pia_write_cb;
  per.tick = cfg->tick != NULL ? pia_tick_cb : NULL;
  per.ctx = &pias[id];
  pias[id].periph_id = theodore_periph_register(&per);
  if (pias[id].periph_id < 0) return -1;
  pias[id].used = 1;
  return id;
}

int theodore_pia_detach(int id)
{
  if (id < 0 || id >= PIA_MAX || !pias[id].used) return -1;
  if (pias[id].cfg.irq_source > 0) theodore_periph_set_irq_source(pias[id].cfg.irq_source, 0);
  theodore_periph_unregister(pias[id].periph_id);
  pias[id].used = 0;
  return 0;
}

void theodore_pia_reset(int id)
{
  Pia *p;
  if (id < 0 || id >= PIA_MAX || !pias[id].used) return;
  p = &pias[id];
  side_reset(&p->s[0]);
  side_reset(&p->s[1]);
  if (p->cfg.irq_source > 0) theodore_periph_set_irq_source(p->cfg.irq_source, 0);
}

void theodore_pia_reset_all(void)
{
  int i;
  for (i = 0; i < PIA_MAX; i++) theodore_pia_reset(i);
}

void theodore_pia_set_pins(int id, ThPiaPort port, uint8_t value)
{
  if (id < 0 || id >= PIA_MAX || !pias[id].used) return;
  pias[id].s[port == THPIA_PORT_B ? 1 : 0].pins = value;
}

void theodore_pia_set_line(int id, ThPiaLine line, int level)
{
  Pia *p;
  Side *s;
  int side, is_c2, old, rising, active;
  uint64_t cycle = theodore_periph_cycle();
  if (id < 0 || id >= PIA_MAX || !pias[id].used) return;
  p = &pias[id];
  side = (line == THPIA_CB1 || line == THPIA_CB2) ? 1 : 0;
  is_c2 = (line == THPIA_CA2 || line == THPIA_CB2);
  s = &p->s[side];
  level = level != 0;
  if (!is_c2)
  {
    old = s->c1;
    s->c1 = (uint8_t) level;
    if (old == level) return;
    rising = level;
    active = ((s->cr & CR_C1_RISING) != 0) == (rising != 0);
    if (active)
    {
      s->cr |= CR_IRQ1_FLAG;
      /* handshake strobe mode (not pulse): an active C1 edge releases C2 */
      if (c2_is_output(s) && !(s->cr & CR_C2_BIT4) && !(s->cr & CR_C2_BIT3))
      {
        s->c2_hs = 1;
        refresh_c2(p, side, cycle);
      }
      recompute_irq(p, cycle);
    }
  }
  else
  {
    old = s->c2_in;
    s->c2_in = (uint8_t) level;
    if (c2_is_output(s) || old == level) { refresh_c2(p, side, cycle); return; }
    s->c2_pin = (uint8_t) level;
    rising = level;
    active = ((s->cr & CR_C2_BIT4) != 0) == (rising != 0);
    if (active)
    {
      s->cr |= CR_IRQ2_FLAG;
      recompute_irq(p, cycle);
    }
  }
}

int theodore_pia_get_state(int id, ThPiaState *st)
{
  const Pia *p;
  if (id < 0 || id >= PIA_MAX || !pias[id].used || st == NULL) return -1;
  p = &pias[id];
  st->ora = p->s[0].or_;  st->ddra = p->s[0].ddr;  st->cra = p->s[0].cr;
  st->orb = p->s[1].or_;  st->ddrb = p->s[1].ddr;  st->crb = p->s[1].cr;
  st->pins_a = p->s[0].pins;  st->pins_b = p->s[1].pins;
  st->ca1 = p->s[0].c1;  st->ca2 = p->s[0].c2_pin;
  st->cb1 = p->s[1].c1;  st->cb2 = p->s[1].c2_pin;
  st->irqa = p->s[0].irq;  st->irqb = p->s[1].irq;
  return 0;
}
