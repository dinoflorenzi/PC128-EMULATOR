/* Dynamic loader for the Theodore libretro core and its theodore_* extension API. */
#ifndef FE_CORE_H
#define FE_CORE_H

#include <stdint.h>
#include <stddef.h>
#include "libretro.h"
#include "periph.h"
#include "pia6821.h"
#include "slots.h"
#include "theodore_dbg.h"

#ifdef CORE_STATIC
/* declared nowhere in the core headers */
#include "theodore_api.h"
THEODORE_API int theodore_key_mapped(unsigned);
THEODORE_API void theodore_key_scancode(unsigned, int);
#endif

/* X(return type, name, (argument types)) */
#define CORE_RETRO_FUNCS(X) \
  X(void,     retro_set_environment,        (retro_environment_t)) \
  X(void,     retro_set_video_refresh,      (retro_video_refresh_t)) \
  X(void,     retro_set_audio_sample,       (retro_audio_sample_t)) \
  X(void,     retro_set_audio_sample_batch, (retro_audio_sample_batch_t)) \
  X(void,     retro_set_input_poll,         (retro_input_poll_t)) \
  X(void,     retro_set_input_state,        (retro_input_state_t)) \
  X(void,     retro_init,                   (void)) \
  X(void,     retro_deinit,                 (void)) \
  X(void,     retro_get_system_info,        (struct retro_system_info *)) \
  X(void,     retro_get_system_av_info,     (struct retro_system_av_info *)) \
  X(void,     retro_reset,                  (void)) \
  X(void,     retro_run,                    (void)) \
  X(size_t,   retro_serialize_size,         (void)) \
  X(bool,     retro_serialize,              (void *, size_t)) \
  X(bool,     retro_unserialize,            (const void *, size_t)) \
  X(bool,     retro_load_game,              (const struct retro_game_info *)) \
  X(void,     retro_unload_game,            (void)) \
  X(void *,   retro_get_memory_data,        (unsigned)) \
  X(size_t,   retro_get_memory_size,        (unsigned))

#define CORE_THEODORE_FUNCS(X) \
  X(int,      theodore_dbg_add_breakpoint,       (TheodoreBpType, uint16_t)) \
  X(int,      theodore_dbg_del_breakpoint,       (TheodoreBpType, uint16_t)) \
  X(void,     theodore_dbg_clear_breakpoints,    (void)) \
  X(unsigned, theodore_dbg_list_breakpoints,     (TheodoreBreakpoint *, unsigned)) \
  X(int,      theodore_dbg_add_data_breakpoint,  (unsigned, uint16_t, uint16_t, const uint8_t *, const uint8_t *, unsigned)) \
  X(int,      theodore_dbg_del_data_breakpoint,  (int)) \
  X(unsigned, theodore_dbg_list_data_breakpoints,(TheodoreDataBreakpoint *, unsigned)) \
  X(void,     theodore_dbg_halt,                 (void)) \
  X(void,     theodore_dbg_resume,               (void)) \
  X(int,      theodore_dbg_step,                 (void)) \
  X(int,      theodore_dbg_is_halted,            (void)) \
  X(unsigned, theodore_dbg_stop_count,           (void)) \
  X(TheodoreStopInfo, theodore_dbg_stop_info,    (void)) \
  X(void,     theodore_dbg_get_regs,             (TheodoreRegs *)) \
  X(void,     theodore_dbg_set_regs,             (const TheodoreRegs *)) \
  X(uint8_t,  theodore_dbg_peek,                 (uint16_t)) \
  X(int,      theodore_dbg_poke,                 (uint16_t, uint8_t)) \
  X(unsigned, theodore_dbg_disasm,               (uint16_t, char *, size_t)) \
  X(void,     theodore_dbg_trace_enable,         (int)) \
  X(void,     theodore_dbg_trace_clear,          (void)) \
  X(unsigned, theodore_dbg_trace_read,           (TheodoreTraceEntry *, unsigned)) \
  X(uint64_t, theodore_periph_cycle,             (void)) \
  X(uint32_t, theodore_periph_irq_mask,          (void)) \
  X(void,     theodore_periph_set_irq_source,    (int, int)) \
  X(void,     theodore_pia_set_pins,             (int, ThPiaPort, uint8_t)) \
  X(void,     theodore_pia_set_line,             (int, ThPiaLine, int)) \
  X(int,      theodore_pia_get_state,            (int, ThPiaState *)) \
  X(int,      theodore_slot_attach,              (ThSlot, const ThSlotConfig *)) \
  X(void,     theodore_slot_detach,              (ThSlot)) \
  X(int,      theodore_slot_is_attached,         (ThSlot)) \
  X(int,      theodore_slot_pia,                 (void)) \
  X(const char *, theodore_slot_error,           (void)) \
  X(int,      theodore_key_mapped,               (unsigned)) \
  X(void,     theodore_key_scancode,             (unsigned, int))

typedef struct
{
  void *lib;
#define X(ret, name, args) ret (*name) args;
  CORE_RETRO_FUNCS(X)
  CORE_THEODORE_FUNCS(X)
#undef X
} CoreApi;

extern CoreApi core;

/* Loads the shared library and resolves every symbol. Returns 0, or -1 with a
   message in err. */
int core_load(const char *path, char *err, size_t err_size);

#endif /* FE_CORE_H */
