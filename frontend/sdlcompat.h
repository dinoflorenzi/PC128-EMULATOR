/* SDL2 / SDL 1.2 compatibility for the small differences (threads, timers, key names).
 * The video, audio, input and joystick code that differs a lot is in main.c under
 * "#if SDL_MAJOR_VERSION == 1". SDL 1.2 is what C4droid's overlay keyboard needs. */
#ifndef SDLCOMPAT_H
#define SDLCOMPAT_H
#include <SDL.h>

#if SDL_MAJOR_VERSION == 1
#define THEODORE_SDL12 1
typedef SDLKey SDL_Keycode;
#define SDLK_KP_0 SDLK_KP0
#define SDLK_KP_1 SDLK_KP1
#define SDLK_KP_2 SDLK_KP2
#define SDLK_KP_3 SDLK_KP3
#define SDLK_KP_4 SDLK_KP4
#define SDLK_KP_5 SDLK_KP5
#define SDLK_KP_6 SDLK_KP6
#define SDLK_KP_7 SDLK_KP7
#define SDLK_KP_8 SDLK_KP8
#define SDLK_KP_9 SDLK_KP9
#define SDL_CreateThread(fn, name, data) SDL_CreateThread(fn, data)
#define SDL_DetachThread(t) ((void) (t))      /* 1.2 threads are never joined here: nothing to do */
#define SDL_zero(x) memset(&(x), 0, sizeof(x))
/* millisecond clock only: enough for frame pacing at 50 Hz */
#define SDL_GetPerformanceCounter() ((Uint64) SDL_GetTicks() * 1000u)
#define SDL_GetPerformanceFrequency() ((Uint64) 1000000u)
#endif

#endif
