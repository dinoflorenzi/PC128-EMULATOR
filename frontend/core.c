#include "core.h"

#include "sdlcompat.h"
#if defined(THEODORE_SDL12) && !defined(CORE_STATIC)
#error "SDL 1.2 has no SDL_LoadObject: build with STATIC=1 (make SDL12=1 does it)"
#endif
#include <stdio.h>

CoreApi core;

int core_load(const char *path, char *err, size_t err_size)
{
  memset(&core, 0, sizeof(core));
#ifdef CORE_STATIC
  /* the core is part of this executable: `path` is ignored */
  (void) path; (void) err; (void) err_size;
#define X(ret, name, args) core.name = name;
  CORE_RETRO_FUNCS(X)
  CORE_THEODORE_FUNCS(X)
#undef X
  return 0;
#else
  core.lib = SDL_LoadObject(path);
  if (core.lib == NULL)
  {
    snprintf(err, err_size, "cannot load core '%s': %s", path, SDL_GetError());
    return -1;
  }
#define X(ret, name, args) \
  core.name = (ret (*) args) SDL_LoadFunction(core.lib, #name); \
  if (core.name == NULL) \
  { \
    snprintf(err, err_size, "core '%s' does not export %s (is it the patched Theodore core?)", path, #name); \
    return -1; \
  }
  CORE_RETRO_FUNCS(X)
  CORE_THEODORE_FUNCS(X)
#undef X
  return 0;
#endif
}
