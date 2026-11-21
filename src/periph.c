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

#include "periph.h"
#include "6809cpu.h"
#include "pia6821.h"
#include "theodore_dbg.h"

#include <string.h>

uint8_t periph_pages[256];

/* A slot is in use while its read callback is non-NULL; ids are slot indexes. */
static TheodorePeripheral periphs[THEODORE_PERIPH_MAX];
static int periph_count = 0; /* high-water mark of used slots */
static uint32_t irq_mask = 0;
static uint64_t cycle_count = 0;

static void rebuild_pages(void)
{
  int i;
  unsigned p;
  memset(periph_pages, 0, sizeof(periph_pages));
  for (i = 0; i < periph_count; i++)
  {
    unsigned first;
    if (periphs[i].read == NULL) continue;
    first = periphs[i].base >> 8;
    unsigned last = ((unsigned) periphs[i].base + periphs[i].size - 1) >> 8;
    if (last > 0xFF) last = 0xFF;
    for (p = first; p <= last; p++) periph_pages[p] = 1;
  }
}

int theodore_periph_register(const TheodorePeripheral *p)
{
  int id;
  if (p == NULL || p->read == NULL || p->write == NULL || p->size == 0
      || (unsigned) p->base + p->size > 0x10000)
  {
    return -1;
  }
  for (id = 0; id < THEODORE_PERIPH_MAX && periphs[id].read != NULL; id++) {}
  if (id >= THEODORE_PERIPH_MAX) return -1;
  periphs[id] = *p;
  if (id >= periph_count) periph_count = id + 1;
  rebuild_pages();
  return id;
}

int theodore_periph_unregister(int id)
{
  if (id < 0 || id >= THEODORE_PERIPH_MAX || periphs[id].read == NULL) return -1;
  memset(&periphs[id], 0, sizeof(periphs[id]));
  while (periph_count > 0 && periphs[periph_count - 1].read == NULL) periph_count--;
  rebuild_pages();
  return 0;
}

void theodore_periph_clear(void)
{
  memset(periphs, 0, sizeof(periphs));
  periph_count = 0;
  irq_mask = 0;
  rebuild_pages();
}

void theodore_periph_set_irq_source(int source, int level)
{
  if (source < 0 || source > 31) return;
  if (level) irq_mask |= (1u << source); else irq_mask &= ~(1u << source);
}

void theodore_periph_set_irq(int level)
{
  theodore_periph_set_irq_source(0, level);
}

uint32_t theodore_periph_irq_mask(void)
{
  return irq_mask;
}

uint64_t theodore_periph_cycle(void)
{
  return cycle_count;
}

static const TheodorePeripheral *find(unsigned short a)
{
  int i;
  for (i = 0; i < periph_count; i++)
  {
    if (periphs[i].read != NULL && a >= periphs[i].base
        && (unsigned) a < (unsigned) periphs[i].base + periphs[i].size)
    {
      return &periphs[i];
    }
  }
  return NULL;
}

bool periph_read(unsigned short a, unsigned char *out)
{
  const TheodorePeripheral *p;
  int v;
  if (tdbg_suppress) return false; /* debugger peeks must not touch hardware */
  p = find(a);
  if (p == NULL) return false;
  v = p->read(p->ctx, a, cycle_count);
  if (v < 0) return false;
  *out = (unsigned char) v;
  return true;
}

bool periph_write(unsigned short a, unsigned char value)
{
  const TheodorePeripheral *p;
  if (tdbg_suppress) return false;
  p = find(a);
  if (p == NULL) return false;
  return p->write(p->ctx, a, value, cycle_count) != 0;
}

void periph_after_instruction(int opcycles)
{
  int i;
  cycle_count += (uint64_t) opcycles;
  for (i = 0; i < periph_count; i++)
  {
    if (periphs[i].read != NULL && periphs[i].tick != NULL) periphs[i].tick(periphs[i].ctx, cycle_count);
  }
  if (irq_mask) Irq();
}

void periph_reset_cycle(void)
{
  int i;
  cycle_count = 0;
  theodore_pia_reset_all();
  for (i = 0; i < periph_count; i++)
  {
    if (periphs[i].read != NULL && periphs[i].reset != NULL) periphs[i].reset(periphs[i].ctx);
  }
}
