/* Services of the frontend used by the debugger console. */
#ifndef FE_APP_H
#define FE_APP_H

#include <stdint.h>
#include <stddef.h>

void app_quit(void);
unsigned app_frame(void);                      /* frames run so far */
const char *app_model(void);                   /* value of the theodore_rom variable */
int  app_screenshot(const char *path);         /* BMP of the last frame; returns 0 or -1 */
void app_reset(void);
int  app_load_content(const char *path);       /* returns 0 or -1 */
int  app_save_state(const char *path);
int  app_load_state(const char *path);
/* Changes a core variable ("theodore_rom", ...). Takes effect at the next frame. */
void app_set_var(const char *key, const char *value);
/* Queues text typed on the Thomson keyboard (see README for the supported characters).
   Escape sequences: \n = Return. Returns the number of characters that have no key. */
int  app_type(const char *text);
int  app_typing(void);                         /* nonzero while typed text is pending */
/* Pastes the clipboard (SDL2) or paste.txt (SDL 1.2) as key presses; typefile does the same with any file.
   Returns 0 or -1. Long text is fed to the keyboard a few characters at a time. */
int  app_paste(void);
int  app_type_file(const char *path);
int  app_press(unsigned retrok, int mods);     /* raw key press (libretro code); mods: 1 Shift, 2 BASIC, 4 RAZ, 8 CNT. 0 or -1 */
int  app_press_scancode(unsigned scancode, int mods);   /* same, by Thomson matrix scancode 0x00-0x4F */
void app_type_stop(void);                      /* drops what is still waiting to be typed */
/* Frames a key is held / released between keys / extra pause after Return. Returns 0 or -1. */
int  app_type_speed(int hold, int gap, int eol);
/* Sets a joypad button: port 0/1, name up|down|left|right|fire|start|select. Returns 0 or -1. */
int  app_set_pad(int port, const char *button, int on);

#endif /* FE_APP_H */
