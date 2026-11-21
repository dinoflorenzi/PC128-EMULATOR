/* Theodore debug frontend: SDL2 host for the patched Theodore libretro core.
 * See README.md. */
#include "sdlcompat.h"
#if (defined(__ANDROID__) || defined(THEODORE_ANDROID)) && !defined(THEODORE_SDL12)
#include <unistd.h>
#ifndef __ANDROID__
extern const char *SDL_AndroidGetExternalStoragePath(void);   /* SDL_system.h declares it only under __ANDROID__ */
#endif
#endif
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "typemap.h"
#include "bridge.h"
#include "core.h"
#include "dbgcli.h"

/* ------------------------------------------------------------------ options */
typedef struct
{
  const char *core_path;
  const char *content;
  const char *model;
  const char *screenshot;      /* written at exit */
  const char *script;
  const char *debug_port;      /* --debug-port [ADDR:]PORT */
  double scale;
  int fullscreen, nearest, no_audio, no_console, turbo, keyjoy, phys_keys, start_halted, text_input;
  long max_frames;             /* 0 = unlimited */
  const char *sets[32];        /* --set key=value */
  int nsets;
  const char *bridges[8];      /* --bridge-exp / -cart / -joy specs, with a kind prefix */
  int nbridges;
} Options;

static Options opt;

static void usage(const char *prog)
{
  printf(
    "Usage: %s [options] [content]\n"
    "  content                   .k7 .fd .sap .rom .m5 .m7 file to load at start\n"
    "  --core FILE               patched Theodore core (default %s)\n"
    "  --model NAME              Auto TO8 TO8D TO9 TO9+ MO5 MO6 PC128 TO7 TO7/70 (default PC128)\n"
    "  --set KEY=VALUE           any core variable, e.g. theodore_autorun=enabled\n"
    "  --scale N                 window scale (default 1.5)    --fullscreen    --nearest\n"
    "  --no-audio  --no-console  --turbo (no frame pacing, no audio)\n"
    "  --no-keyjoy               do not use the numeric keypad as joystick 0\n"
    "  --phys-keys               map keys by physical position (US layout) instead of by character\n"
    "  --text-input              characters come from SDL text input (on-screen keyboard; default on Android)\n"
    "  --kbd-icon                draw a keyboard button top-right (tap = core virtual keyboard; default on Android)\n"
    "  --log FILE                write stdout/stderr to FILE (no console on Android)\n"
    "  --debug-port [ADDR:]PORT  debugger console over TCP (for Android); ADDR defaults to 127.0.0.1\n"
    "  --script FILE             run debugger commands from FILE at start\n"
    "  --halt                    start halted\n"
    "  --frames N                exit after N frames        --screenshot FILE.bmp  (written at exit)\n"
    "  --bridge-exp  HOST:PORT@BASE+SIZE[,irq=N][,poll=CYCLES][,override]\n"
    "  --bridge-cart HOST:PORT@BASE+SIZE[,irq=N][,poll=CYCLES][,ctl]\n"
    "  --bridge-joy  HOST:PORT[,irq=N][,poll=CYCLES][,builtin][,std]\n"
    "  (BASE and SIZE are hexadecimal; see BRIDGE_PROTOCOL.md)\n"
    "Hotkeys (Alt held): Enter fullscreen, P halt/resume, S step, R reset, C screenshot,\n"
    "  F fast-forward, 1 save state, 2 load state, Q quit.\n", prog,
#ifdef _WIN32
    "theodore_libretro.dll"
#else
    "./theodore_libretro.so"
#endif
  );
}

/* ----------------------------------------------------------- core variables */
#define MAX_VARS 64
typedef struct { char key[64]; char value[64]; char def[64]; } Var;
static Var vars[MAX_VARS];
static int nvars = 0;
static int vars_updated = 0;

static Var *find_var(const char *key)
{
  int i;
  for (i = 0; i < nvars; i++) if (strcmp(vars[i].key, key) == 0) return &vars[i];
  return NULL;
}

static void register_vars(const struct retro_variable *list)
{
  for (; list != NULL && list->key != NULL && nvars < MAX_VARS; list++)
  {
    /* "Description; opt1|opt2|opt3": the default is the first option */
    const char *semi = list->value ? strchr(list->value, ';') : NULL;
    const char *p = semi ? semi + 1 : "";
    size_t n = 0;
    Var *v = &vars[nvars++];
    while (*p == ' ') p++;
    while (p[n] != '\0' && p[n] != '|') n++;
    snprintf(v->key, sizeof(v->key), "%s", list->key);
    snprintf(v->def, sizeof(v->def), "%.*s", (int) n, p);
    memcpy(v->value, v->def, sizeof(v->value));
  }
}

void app_set_var(const char *key, const char *value)
{
  Var *v = find_var(key);
  char tmp[64];
  if (v == NULL)
  {
    printf("unknown core variable '%s'\n", key);
    return;
  }
  snprintf(tmp, sizeof(tmp), "%s", value); /* value may point into vars[] */
  memcpy(v->value, tmp, sizeof(v->value));
  vars_updated = 1;
}

const char *app_model(void)
{
  Var *v = find_var("theodore_rom");
  return v ? v->value : "?";
}

/* ------------------------------------------------------------------- state */
static int quit_flag = 0;
static unsigned frame_count = 0;
#ifdef THEODORE_SDL12
static SDL_Surface *screen = NULL, *scaled = NULL;
static int audio_dev = 0;
#define TOUCH_MOUSE(w) 0
#else
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_AudioDeviceID audio_dev = 0;
#define TOUCH_MOUSE(w) ((w) == SDL_TOUCH_MOUSEID)
#endif
static struct retro_system_av_info av;
static retro_keyboard_event_t kb_callback = NULL;
static uint16_t *framebuf = NULL;
static unsigned fb_w = 0, fb_h = 0;
static size_t fb_pitch = 0;
static SDL_Rect dest;                  /* where the frame is drawn in the window */
static int16_t pad[2][16];             /* libretro joypad state per port */
static int mouse_x = 0, mouse_y = 0, mouse_down = 0; /* light pen, in libretro pointer units */
static int fast_forward = 0;
static int pending_load_state = 0;

void app_quit(void) { quit_flag = 1; }
unsigned app_frame(void) { return frame_count; }

/* ------------------------------------------------------------ libretro glue */
static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
  va_list ap;
  static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
  fprintf(stderr, "[core %s] ", (unsigned) level < 4 ? names[level] : "?");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
  switch (cmd)
  {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *) data)->log = log_cb;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return *(enum retro_pixel_format *) data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
      register_vars((const struct retro_variable *) data);
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
      struct retro_variable *rv = (struct retro_variable *) data;
      Var *v = rv->key ? find_var(rv->key) : NULL;
      if (v == NULL) return false;
      rv->value = v->value;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *) data = vars_updated != 0;
      vars_updated = 0;
      return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
      kb_callback = ((struct retro_keyboard_callback *) data)->callback;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
      fprintf(stderr, "[core message] %s\n", ((struct retro_message *) data)->msg);
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *(unsigned *) data = RETRO_LANGUAGE_ENGLISH;
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      return true;
    default:
      return false;
  }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
  if (data == NULL) return;
  if (framebuf == NULL || w != fb_w || h != fb_h)
  {
    free(framebuf);
    framebuf = (uint16_t *) malloc(pitch * h);
    fb_w = w; fb_h = h; fb_pitch = pitch;
  }
  memcpy(framebuf, data, pitch * h);
}

#ifdef THEODORE_SDL12
/* SDL 1.2 has no SDL_QueueAudio: the core pushes into a ring, the audio callback drains it */
#define ARING 65536u
static Uint8 aring[ARING];
static volatile unsigned ar_r = 0, ar_w = 0;   /* running byte counters */

static void audio_fill(void *ud, Uint8 *stream, int len)
{
  unsigned avail = ar_w - ar_r, n = (unsigned) len < avail ? (unsigned) len : avail, i;
  (void) ud;
  for (i = 0; i < n; i++) stream[i] = aring[(ar_r + i) & (ARING - 1)];
  ar_r += n;
  if (n < (unsigned) len) memset(stream + n, 0, (size_t) len - n);
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
  if (audio_dev != 0 && !fast_forward)
  {
    unsigned bytes = (unsigned) (frames * 4), i;
    unsigned limit = (unsigned) (av.timing.sample_rate / av.timing.fps * 4 * 6);
    if (limit > ARING / 2) limit = ARING / 2;
    if (bytes > limit) bytes = limit;
    SDL_LockAudio();
    if (ar_w - ar_r + bytes > limit) ar_r = ar_w;   /* host fell behind: drop what is queued */
    for (i = 0; i < bytes; i++) aring[(ar_w + i) & (ARING - 1)] = ((const Uint8 *) data)[i];
    ar_w += bytes;
    SDL_UnlockAudio();
  }
  return frames;
}
#else
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
  if (audio_dev != 0 && !fast_forward)
  {
    /* keep at most ~6 frames queued: drop everything if the host falls behind */
    if (SDL_GetQueuedAudioSize(audio_dev) > (Uint32) (av.timing.sample_rate / av.timing.fps * 4 * 6))
    {
      SDL_ClearQueuedAudio(audio_dev);
    }
    SDL_QueueAudio(audio_dev, data, (Uint32) (frames * 4));
  }
  return frames;
}
#endif

static void audio_cb(int16_t l, int16_t r) { (void) l; (void) r; }
static void poll_cb(void) {}

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
  (void) index;
  if (device == RETRO_DEVICE_JOYPAD)
  {
    return (port < 2 && id < 16) ? pad[port][id] : 0;
  }
  if (device == RETRO_DEVICE_POINTER && port == 2)
  {
    switch (id)
    {
      case RETRO_DEVICE_ID_POINTER_X: return (int16_t) mouse_x;
      case RETRO_DEVICE_ID_POINTER_Y: return (int16_t) mouse_y;
      case RETRO_DEVICE_ID_POINTER_PRESSED: return (int16_t) mouse_down;
      default: return 0;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ keyboard */
/* SDL keycode -> libretro key code. For printable ASCII the two are the same;
   the special keys are translated explicitly. */
static unsigned sdl_to_retrok(SDL_Keycode k)
{
  if (k > 0 && k < 127) return (unsigned) k;
  switch (k)
  {
    case SDLK_UP: return RETROK_UP;           case SDLK_DOWN: return RETROK_DOWN;
    case SDLK_LEFT: return RETROK_LEFT;       case SDLK_RIGHT: return RETROK_RIGHT;
    case SDLK_HOME: return RETROK_HOME;       case SDLK_END: return RETROK_END;
    case SDLK_INSERT: return RETROK_INSERT;   case SDLK_DELETE: return RETROK_DELETE;
    case SDLK_PAGEUP: return RETROK_PAGEUP;   case SDLK_PAGEDOWN: return RETROK_PAGEDOWN;
    case SDLK_F1: return RETROK_F1;  case SDLK_F2: return RETROK_F2;  case SDLK_F3: return RETROK_F3;
    case SDLK_F4: return RETROK_F4;  case SDLK_F5: return RETROK_F5;  case SDLK_F6: return RETROK_F6;
    case SDLK_F7: return RETROK_F7;  case SDLK_F8: return RETROK_F8;  case SDLK_F9: return RETROK_F9;
    case SDLK_F10: return RETROK_F10; case SDLK_F11: return RETROK_F11; case SDLK_F12: return RETROK_F12;
    case SDLK_CAPSLOCK: return RETROK_CAPSLOCK;
    case SDLK_LSHIFT: return RETROK_LSHIFT;   case SDLK_RSHIFT: return RETROK_RSHIFT;
    case SDLK_LCTRL: return RETROK_LCTRL;     case SDLK_RCTRL: return RETROK_RCTRL;
    case SDLK_LALT: return RETROK_LALT;       case SDLK_RALT: return RETROK_RALT;
    case SDLK_KP_0: return RETROK_KP0; case SDLK_KP_1: return RETROK_KP1; case SDLK_KP_2: return RETROK_KP2;
    case SDLK_KP_3: return RETROK_KP3; case SDLK_KP_4: return RETROK_KP4; case SDLK_KP_5: return RETROK_KP5;
    case SDLK_KP_6: return RETROK_KP6; case SDLK_KP_7: return RETROK_KP7; case SDLK_KP_8: return RETROK_KP8;
    case SDLK_KP_9: return RETROK_KP9;
    case SDLK_KP_PERIOD: return RETROK_KP_PERIOD; case SDLK_KP_DIVIDE: return RETROK_KP_DIVIDE;
    case SDLK_KP_MULTIPLY: return RETROK_KP_MULTIPLY; case SDLK_KP_MINUS: return RETROK_KP_MINUS;
    case SDLK_KP_PLUS: return RETROK_KP_PLUS; case SDLK_KP_ENTER: return RETROK_KP_ENTER;
    default: return 0;
  }
}

/* Physical-position mapping: the key that sits where a US keyboard has the
   given character, whatever the host layout. */
#ifndef THEODORE_SDL12
static unsigned scancode_to_retrok(SDL_Scancode sc)
{
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (unsigned) ('a' + (sc - SDL_SCANCODE_A));
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (unsigned) ('1' + (sc - SDL_SCANCODE_1));
  switch (sc)
  {
    case SDL_SCANCODE_0: return '0';          case SDL_SCANCODE_RETURN: return RETROK_RETURN;
    case SDL_SCANCODE_ESCAPE: return RETROK_ESCAPE; case SDL_SCANCODE_BACKSPACE: return RETROK_BACKSPACE;
    case SDL_SCANCODE_TAB: return RETROK_TAB; case SDL_SCANCODE_SPACE: return ' ';
    case SDL_SCANCODE_MINUS: return '-';      case SDL_SCANCODE_EQUALS: return '=';
    case SDL_SCANCODE_LEFTBRACKET: return '[';case SDL_SCANCODE_RIGHTBRACKET: return ']';
    case SDL_SCANCODE_BACKSLASH: return '\\'; case SDL_SCANCODE_SEMICOLON: return ';';
    case SDL_SCANCODE_APOSTROPHE: return '\'';case SDL_SCANCODE_GRAVE: return '`';
    case SDL_SCANCODE_COMMA: return ',';      case SDL_SCANCODE_PERIOD: return '.';
    case SDL_SCANCODE_SLASH: return '/';
    default: return sdl_to_retrok(SDL_GetKeyFromScancode(sc));
  }
}
#endif

/* Queue entries >= SCAN_BASE are Thomson matrix scancodes (SCAN_BASE + scancode), not libretro key codes */
#define SCAN_BASE 0x10000u

static void send_key(unsigned retrok, int down)
{
  if (retrok >= SCAN_BASE) core.theodore_key_scancode(retrok - SCAN_BASE, down);
  else if (kb_callback != NULL && retrok != 0) kb_callback(down != 0, retrok, 0, 0);
}

/* Numeric-keypad joystick: 8/2/4/6 move, 0 or 5 fire (joystick 0) */
static int keyjoy_key(SDL_Keycode k, int down)
{
  int id = -1;
  switch (k)
  {
    case SDLK_KP_8: id = RETRO_DEVICE_ID_JOYPAD_UP; break;
    case SDLK_KP_2: id = RETRO_DEVICE_ID_JOYPAD_DOWN; break;
    case SDLK_KP_4: id = RETRO_DEVICE_ID_JOYPAD_LEFT; break;
    case SDLK_KP_6: id = RETRO_DEVICE_ID_JOYPAD_RIGHT; break;
    case SDLK_KP_0: case SDLK_KP_5: id = RETRO_DEVICE_ID_JOYPAD_B; break;
    default: return 0;
  }
  pad[0][id] = down ? 1 : 0;
  return 1;
}

int app_set_pad(int port, const char *button, int on)
{
  static const struct { const char *name; int id; } names[] = {
    { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
    { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { "fire", RETRO_DEVICE_ID_JOYPAD_B }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
    { "select", RETRO_DEVICE_ID_JOYPAD_SELECT } };
  size_t i;
  if (port < 0 || port > 1) return -1;
  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
  {
    if (strcmp(names[i].name, button) == 0) { pad[port][names[i].id] = on ? 1 : 0; return 0; }
  }
  return -1;
}

/* ------------------------------------------------------------- typed input */
typedef struct { unsigned retrok; int down; } KeyEvt;
#define TYPEQ 8192
static KeyEvt typeq[TYPEQ];
static int type_head = 0, type_tail = 0, type_wait = 0;

static void type_push(unsigned retrok, int down)
{
  int next = (type_tail + 1) % TYPEQ;
  if (next == type_head) return;
  typeq[type_tail].retrok = retrok;
  typeq[type_tail].down = down;
  type_tail = next;
}

/* Pasted / file text is not queued at once (a long listing would overflow the queue): it waits in
   paste_buf and the pump refills the queue a few characters at a time. */
static char *paste_buf = NULL;
static size_t paste_len = 0, paste_pos = 0;
static int paste_unmapped = 0, paste_total = 0;
static int type_hold = 3, type_gap = 1, type_eol = 30;   /* frames: key held, released between keys, extra after Return */

int app_typing(void) { return type_head != type_tail || paste_pos < paste_len; }

/* Characters that could not be typed (distinct, in order of appearance), for the report */
static char skipped[24];

static void note_skipped(char c)
{
  size_t n = strlen(skipped);
  if (n < sizeof(skipped) - 2 && strchr(skipped, c) == NULL) { skipped[n] = c; skipped[n + 1] = '\0'; }
}

/* Thomson matrix scancodes measured per model (tools/calibrate_typemap.py). Other models use the libretro
   key codes, which follow the position of the key, so only letters, digits and space are reliable there. */
static const TypeKey *typemap_for_model(void)
{
  const char *m = app_model();
  if (!strcmp(m, "PC128")) return typemap_pc128;
  if (!strcmp(m, "MO6")) return typemap_mo6;
  return NULL;
}

/* Each character: [shift down] key down, hold, key up, release gap. Returns 1 if it has no key. */
static int type_char(char c)
{
  unsigned key, shift = 0;
  const TypeKey *tm = typemap_for_model();
  if (tm != NULL)
  {
    if (c == '\n') key = SCAN_BASE + 0x26;
    else if (c >= 32 && c < 127 && tm[c - 32].sc >= 0) { key = SCAN_BASE + (unsigned) tm[c - 32].sc; shift = tm[c - 32].shift; }
    else { note_skipped(c); return 1; }
    if (shift) type_push(SCAN_BASE + 0x07, 1);
    type_push(key, 1);
    type_push(0, 1);
    type_push(key, 0);
    if (shift) type_push(SCAN_BASE + 0x07, 0);
    type_push(0, 0);
    if (c == '\n' && type_eol > 0) type_push(0, 2);
    return 0;
  }
  if (c == '\n') key = RETROK_RETURN;
  else if (c >= 'A' && c <= 'Z') { key = (unsigned) (c - 'A' + 'a'); shift = 1; }
  else key = (unsigned char) c;
  if (!core.theodore_key_mapped(key)) { note_skipped(c); return 1; }
  if (shift) type_push(RETROK_LSHIFT, 1);
  type_push(key, 1);
  type_push(0, 1);   /* 0 = wait marker, handled by the pump (hold) */
  type_push(key, 0);
  if (shift) type_push(RETROK_LSHIFT, 0);
  type_push(0, 0);   /* release gap */
  if (c == '\n' && type_eol > 0) type_push(0, 2);   /* extra pause: BASIC is busy with the line */
  return 0;
}

int app_type(const char *text)
{
  int unmapped = 0;
  for (; *text != '\0'; text++)
  {
    char c = *text;
    if (c == '\\' && text[1] == 'n') { c = '\n'; text++; }
    unmapped += type_char(c);
  }
  return unmapped;
}

/* One raw key press (libretro key code) with modifiers: bit 0 Shift, 1 BASIC key (right shift), 2 RAZ (Alt), 3 CNT (Ctrl).
   Returns -1 if the key has no Thomson key. */
static const unsigned mod_keys[4] = { RETROK_LSHIFT, RETROK_RSHIFT, RETROK_LALT, RETROK_LCTRL };

int app_press(unsigned retrok, int mods)
{
  int i;
  if (!core.theodore_key_mapped(retrok)) return -1;
  for (i = 0; i < 4; i++) if (mods & (1 << i)) type_push(mod_keys[i], 1);
  type_push(retrok, 1);
  type_push(0, 1);
  type_push(retrok, 0);
  for (i = 3; i >= 0; i--) if (mods & (1 << i)) type_push(mod_keys[i], 0);
  type_push(0, 0);
  return 0;
}

/* Same as app_press() for a Thomson matrix scancode (0x00-0x4F): reaches keys that have no libretro code. */
int app_press_scancode(unsigned scancode, int mods)
{
  int i;
  if (scancode >= 0x50) return -1;
  for (i = 0; i < 4; i++) if (mods & (1 << i)) type_push(mod_keys[i], 1);
  type_push(SCAN_BASE + scancode, 1);
  type_push(0, 1);
  type_push(SCAN_BASE + scancode, 0);
  for (i = 3; i >= 0; i--) if (mods & (1 << i)) type_push(mod_keys[i], 0);
  type_push(0, 0);
  return 0;
}

static void type_stop(void)
{
  type_head = type_tail = 0;
  type_wait = 0;
  free(paste_buf);
  paste_buf = NULL; paste_len = paste_pos = 0;
}

void app_type_stop(void)
{
  /* a key already pressed has its release still in the queue: send the releases, or the Thomson sees it held */
  while (type_head != type_tail)
  {
    KeyEvt e = typeq[type_head];
    type_head = (type_head + 1) % TYPEQ;
    if (e.retrok != 0 && !e.down) send_key(e.retrok, 0);
  }
  type_stop();
}

int app_type_speed(int hold, int gap, int eol)
{
  if (hold < 0 || hold > 20 || gap < 0 || gap > 20 || eol < 0 || eol > 100) return -1;
  type_hold = hold; type_gap = gap; type_eol = eol;
  return 0;
}

/* Normalises clipboard text: CRLF/CR -> one Return, tab -> space, typographic characters -> ASCII;
   anything else outside ASCII is dropped and counted. */
static char *paste_normalize(const char *in, size_t n, size_t *outlen, int *dropped)
{
  char *out = (char *) malloc(n * 3 + 1);
  size_t i, o = 0;
  *dropped = 0;
  if (out == NULL) return NULL;
  for (i = 0; i < n; i++)
  {
    unsigned char c = (unsigned char) in[i];
    if (c == '\r') { out[o++] = '\n'; if (i + 1 < n && in[i + 1] == '\n') i++; }
    else if (c == '\t') out[o++] = ' ';
    else if (c == '\n' || (c >= 32 && c < 127)) out[o++] = (char) c;
    else if (c == 0xC2 && i + 1 < n && (unsigned char) in[i + 1] == 0xA0) { out[o++] = ' '; i++; }
    else if (c == 0xE2 && i + 2 < n && (unsigned char) in[i + 1] == 0x80)
    {
      unsigned char d = (unsigned char) in[i + 2];
      i += 2;
      if (d == 0x98 || d == 0x99) out[o++] = '\'';
      else if (d == 0x9C || d == 0x9D) out[o++] = '"';
      else if (d == 0x93 || d == 0x94) out[o++] = '-';
      else if (d == 0xA6) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
      else (*dropped)++;
    }
    else if (c >= 0xC0) (*dropped)++;       /* lead byte of another UTF-8 character */
    else if (c < 0x80) { /* other control characters: ignored */ }
    /* continuation bytes (0x80-0xBF) belong to a character already counted */
  }
  out[o] = '\0';
  *outlen = o;
  return out;
}

/* Starts typing `text` (len bytes). Takes the text as it is, including the "\n" sequence. */
static int paste_text(const char *text, size_t len, const char *what)
{
  size_t olen, rest;
  int dropped;
  char *norm = paste_normalize(text, len, &olen, &dropped);
  char *nb;
  if (norm == NULL) return -1;
  rest = paste_len - paste_pos;
  nb = (char *) malloc(rest + olen + 1);
  if (nb == NULL) { free(norm); return -1; }
  memcpy(nb, paste_buf + paste_pos, rest);       /* text still waiting from an earlier paste comes first */
  memcpy(nb + rest, norm, olen);
  nb[rest + olen] = '\0';
  free(norm);
  free(paste_buf);
  paste_buf = nb; paste_len = rest + olen; paste_pos = 0;
  paste_total += (int) olen;
  printf("%s: %lu characters to type%s\n", what, (unsigned long) olen, dropped ? " (some non-ASCII characters dropped)" : "");
  if (dropped) printf("%d non-ASCII character(s) skipped\n", dropped);
  if (typemap_for_model() != NULL && strpbrk(nb + rest, "abcdefghijklmnopqrstuvwxyz") != NULL)
    printf("note: lower case letters type as capitals while Caps Lock is on; press the Caps Lock key first to keep the case\n");
  return 0;
}

static void type_pump(void)
{
  if (type_wait > 0) { type_wait--; return; }
  if (type_head == type_tail && paste_pos < paste_len)
  {
    int n;
    for (n = 0; n < 32 && paste_pos < paste_len; n++) paste_unmapped += type_char(paste_buf[paste_pos++]);
    if (paste_pos >= paste_len)
    {
      free(paste_buf); paste_buf = NULL; paste_len = paste_pos = 0;
      if (paste_unmapped) printf("paste: %d character(s) have no key on this model and were skipped: %s\n", paste_unmapped, skipped);
      paste_unmapped = 0; paste_total = 0; skipped[0] = '\0';
    }
  }
  while (type_head != type_tail)
  {
    KeyEvt e = typeq[type_head];
    type_head = (type_head + 1) % TYPEQ;
    if (e.retrok == 0) { type_wait = e.down == 2 ? type_eol : (e.down ? type_hold : type_gap); return; }
    send_key(e.retrok, e.down);
  }
}

/* Clipboard (SDL2) or the file paste.txt (SDL 1.2 has no clipboard API; also usable from the debugger). */
static char *read_whole_file(const char *path, size_t *n)
{
  FILE *f = fopen(path, "rb");
  char *buf;
  long sz;
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz < 0 || sz > 4 * 1024 * 1024 || (buf = (char *) malloc((size_t) sz + 1)) == NULL) { fclose(f); return NULL; }
  *n = fread(buf, 1, (size_t) sz, f);
  fclose(f);
  buf[*n] = '\0';
  return buf;
}

int app_type_file(const char *path)
{
  size_t n;
  char *t = read_whole_file(path, &n);
  int rc;
  if (t == NULL) { printf("cannot read '%s' (max 4 MB)\n", path); return -1; }
  rc = paste_text(t, n, "typefile");
  free(t);
  return rc;
}

int app_paste(void)
{
  int rc;
#ifdef THEODORE_SDL12
  static const char *paths[] = { "paste.txt", "/sdcard/paste.txt" };
  size_t k, n = 0;
  char *t = NULL;
  for (k = 0; k < sizeof(paths) / sizeof(paths[0]) && t == NULL; k++) t = read_whole_file(paths[k], &n);
  if (t == NULL) { printf("paste: SDL 1.2 has no clipboard access: put the text in paste.txt (or /sdcard/paste.txt)\n"); return -1; }
  rc = paste_text(t, n, "paste (paste.txt)");
  free(t);
#else
  char *t;
  if (!SDL_HasClipboardText()) { printf("paste: the clipboard has no text\n"); return -1; }
  t = SDL_GetClipboardText();
  if (t == NULL || t[0] == '\0') { printf("paste: the clipboard has no text\n"); if (t) SDL_free(t); return -1; }
  rc = paste_text(t, strlen(t), "paste");
  SDL_free(t);
#endif
  return rc;
}

/* ----------------------------------------------------------- video / window */
static void compute_dest(int ww, int wh)
{
  float aspect = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio
                                              : (float) fb_w / (float) (fb_h ? fb_h : 1);
  int w = ww, h = (int) (ww / aspect);
  if (h > wh) { h = wh; w = (int) (wh * aspect); }
  dest.w = w; dest.h = h;
  dest.x = (ww - w) / 2; dest.y = (wh - h) / 2;
}

static int kbd_icon = 0;   /* draw a small keyboard button in the top-right corner */

#ifdef THEODORE_SDL12
/* software presentation: scale the RGB565 frame (nearest neighbour) into a surface of the
   destination size, then blit it to the screen (SDL converts if the screen is not RGB565) */
static void present(void)
{
  int x, y, ww, wh;
  static int xmap[8192];
  SDL_Rect d;
  if (framebuf == NULL || screen == NULL) return;
  ww = screen->w; wh = screen->h;
  compute_dest(ww, wh);
  if (dest.w < 1 || dest.h < 1 || dest.w > 8192) return;
  if (scaled == NULL || scaled->w != dest.w || scaled->h != dest.h)
  {
    if (scaled != NULL) SDL_FreeSurface(scaled);
    scaled = SDL_CreateRGBSurface(SDL_SWSURFACE, dest.w, dest.h, 16, 0xF800, 0x07E0, 0x001F, 0);
    if (scaled == NULL) return;
  }
  for (x = 0; x < dest.w; x++) xmap[x] = (int) ((long) x * (long) fb_w / dest.w);
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  for (y = 0; y < dest.h; y++)
  {
    const uint16_t *src = (const uint16_t *) ((const char *) framebuf + (size_t) ((long) y * (long) fb_h / dest.h) * fb_pitch);
    uint16_t *dst = (uint16_t *) ((Uint8 *) scaled->pixels + (size_t) y * scaled->pitch);
    for (x = 0; x < dest.w; x++) dst[x] = src[xmap[x]];
  }
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  SDL_FillRect(screen, NULL, 0);
  d = dest;
  SDL_BlitSurface(scaled, NULL, screen, &d);
  if (kbd_icon)
  {
    SDL_Rect b;
    b.x = (Sint16) (ww * 85 / 100); b.y = 0; b.w = (Uint16) (ww - b.x); b.h = (Uint16) (wh * 15 / 100);
    SDL_FillRect(screen, &b, SDL_MapRGB(screen->format, 200, 200, 200));
    b.x = 0; b.w = (Uint16) (ww * 15 / 100);
    SDL_FillRect(screen, &b, SDL_MapRGB(screen->format, 120, 200, 120));   /* top-left: paste */
  }
  SDL_Flip(screen);
}
#else
static void present(void)
{
  int ww, wh;
  if (framebuf == NULL || texture == NULL) return;
  SDL_UpdateTexture(texture, NULL, framebuf, (int) fb_pitch);
  SDL_GetRendererOutputSize(renderer, &ww, &wh);
  compute_dest(ww, wh);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, &dest);
  if (kbd_icon)
  {
    SDL_Rect b, k;
    int r, c, kw, kh;
    b.x = ww * 85 / 100; b.y = 0; b.w = ww - b.x; b.h = wh * 15 / 100;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 200);
    SDL_RenderFillRect(renderer, &b);
    SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
    SDL_RenderDrawRect(renderer, &b);
    kw = b.w / 6; kh = b.h / 6;
    if (kw < 1) kw = 1;
    if (kh < 1) kh = 1;
    for (r = 0; r < 3; r++)
      for (c = 0; c < 4; c++)
      {
        k.x = b.x + kw / 2 + c * (kw + kw / 4 > 0 ? kw + kw / 4 : 1); k.y = b.y + kh + r * (kh + kh / 2); k.w = kw; k.h = kh;
        SDL_RenderFillRect(renderer, &k);
      }
    b.x = 0; b.w = ww * 15 / 100;                       /* top-left: paste */
    SDL_SetRenderDrawColor(renderer, 40, 120, 40, 200);
    SDL_RenderFillRect(renderer, &b);
    SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
    SDL_RenderDrawRect(renderer, &b);
  }
  SDL_RenderPresent(renderer);
}
#endif

int app_screenshot(const char *path)
{
  SDL_Surface *s, *rgb;
  int rc;
  if (framebuf == NULL) return -1;
  /* RGB565 masks; SDL_CreateRGBSurfaceWithFormatFrom needs SDL >= 2.0.5 (older on some Android builds) */
  s = SDL_CreateRGBSurfaceFrom(framebuf, (int) fb_w, (int) fb_h, 16, (int) fb_pitch, 0xF800, 0x07E0, 0x001F, 0);
  if (s == NULL) return -1;
#ifdef THEODORE_SDL12
  rgb = SDL_CreateRGBSurface(SDL_SWSURFACE, (int) fb_w, (int) fb_h, 24, 0xFF0000, 0x00FF00, 0x0000FF, 0);
  if (rgb != NULL) SDL_BlitSurface(s, NULL, rgb, NULL);
#else
  rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
#endif
  SDL_FreeSurface(s);
  if (rgb == NULL) return -1;
  rc = SDL_SaveBMP(rgb, path);
  SDL_FreeSurface(rgb);
  return rc;
}

/* ------------------------------------------------------------ core services */
void app_reset(void)
{
  core.retro_reset();
}

int app_load_content(const char *path)
{
  struct retro_game_info gi;
  FILE *probe = fopen(path, "rb");
  if (probe == NULL)
  {
    printf("cannot open '%s'\n", path); /* the core would only log the error and report success */
    return -1;
  }
  fclose(probe);
  memset(&gi, 0, sizeof(gi));
  gi.path = path;
  core.retro_unload_game();
  if (!core.retro_load_game(&gi))
  {
    printf("the core refused '%s'\n", path);
    return -1;
  }
  return 0;
}

int app_save_state(const char *path)
{
  size_t n = core.retro_serialize_size();
  void *buf = malloc(n);
  FILE *f;
  int rc = -1;
  if (buf != NULL && core.retro_serialize(buf, n) && (f = fopen(path, "wb")) != NULL)
  {
    rc = fwrite(buf, 1, n, f) == n ? 0 : -1;
    fclose(f);
  }
  free(buf);
  return rc;
}

int app_load_state(const char *path)
{
  size_t n = core.retro_serialize_size();
  void *buf = malloc(n);
  FILE *f;
  int rc = -1;
  if (buf != NULL && (f = fopen(path, "rb")) != NULL)
  {
    if (fread(buf, 1, n, f) == n && core.retro_unserialize(buf, n)) rc = 0;
    fclose(f);
  }
  free(buf);
  return rc;
}

/* ------------------------------------------------------------------ events */
#ifdef THEODORE_SDL12
static SDL_Joystick *controllers[2];

static void open_controllers(void)
{
  int i, slot = 0;
  for (i = 0; i < SDL_NumJoysticks() && slot < 2; i++)
  {
    if (controllers[slot] == NULL)
    {
      controllers[slot] = SDL_JoystickOpen(i);
      if (controllers[slot] != NULL) slot++;
    }
  }
}

/* generic joystick: hat or first stick = directions, button 0 or 1 = fire, 6/7 = select/start */
static void poll_controllers(void)
{
  int p;
  SDL_JoystickUpdate();
  for (p = 0; p < 2; p++)
  {
    SDL_Joystick *j = controllers[p];
    int hat, x, y, nb, id;
    int on[16];
    if (j == NULL) continue;
    hat = SDL_JoystickNumHats(j) > 0 ? SDL_JoystickGetHat(j, 0) : 0;
    x = SDL_JoystickNumAxes(j) > 0 ? SDL_JoystickGetAxis(j, 0) : 0;
    y = SDL_JoystickNumAxes(j) > 1 ? SDL_JoystickGetAxis(j, 1) : 0;
    nb = SDL_JoystickNumButtons(j);
    memset(on, 0, sizeof(on));
    on[RETRO_DEVICE_ID_JOYPAD_UP] = (hat & SDL_HAT_UP) || y < -16000;
    on[RETRO_DEVICE_ID_JOYPAD_DOWN] = (hat & SDL_HAT_DOWN) || y > 16000;
    on[RETRO_DEVICE_ID_JOYPAD_LEFT] = (hat & SDL_HAT_LEFT) || x < -16000;
    on[RETRO_DEVICE_ID_JOYPAD_RIGHT] = (hat & SDL_HAT_RIGHT) || x > 16000;
    on[RETRO_DEVICE_ID_JOYPAD_B] = (nb > 0 && SDL_JoystickGetButton(j, 0)) || (nb > 1 && SDL_JoystickGetButton(j, 1));
    on[RETRO_DEVICE_ID_JOYPAD_SELECT] = nb > 6 && SDL_JoystickGetButton(j, 6);
    on[RETRO_DEVICE_ID_JOYPAD_START] = nb > 7 && SDL_JoystickGetButton(j, 7);
    for (id = 0; id < 16; id++)
    {
      if (id != RETRO_DEVICE_ID_JOYPAD_UP && id != RETRO_DEVICE_ID_JOYPAD_DOWN && id != RETRO_DEVICE_ID_JOYPAD_LEFT
          && id != RETRO_DEVICE_ID_JOYPAD_RIGHT && id != RETRO_DEVICE_ID_JOYPAD_B
          && id != RETRO_DEVICE_ID_JOYPAD_SELECT && id != RETRO_DEVICE_ID_JOYPAD_START) continue;
      if (p == 0 && opt.keyjoy && pad[0][id] && !on[id] && id <= RETRO_DEVICE_ID_JOYPAD_RIGHT) continue;
      pad[p][id] = (int16_t) on[id];
    }
  }
}
#else
static SDL_GameController *controllers[2];

static void open_controllers(void)
{
  int i, slot = 0;
  for (i = 0; i < SDL_NumJoysticks() && slot < 2; i++)
  {
    if (SDL_IsGameController(i) && controllers[slot] == NULL)
    {
      controllers[slot] = SDL_GameControllerOpen(i);
      if (controllers[slot] != NULL) slot++;
    }
  }
}

static void poll_controllers(void)
{
  int p;
  static const struct { SDL_GameControllerButton b; int id; } map[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { SDL_CONTROLLER_BUTTON_A, RETRO_DEVICE_ID_JOYPAD_B },
    { SDL_CONTROLLER_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START },
    { SDL_CONTROLLER_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT },
  };
  for (p = 0; p < 2; p++)
  {
    size_t i;
    if (controllers[p] == NULL) continue;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
      int on = SDL_GameControllerGetButton(controllers[p], map[i].b);
      if (p == 0 && opt.keyjoy && pad[0][map[i].id] && !on && map[i].id <= RETRO_DEVICE_ID_JOYPAD_RIGHT) continue;
      pad[p][map[i].id] = (int16_t) on;
    }
    {
      int x = SDL_GameControllerGetAxis(controllers[p], SDL_CONTROLLER_AXIS_LEFTX);
      int y = SDL_GameControllerGetAxis(controllers[p], SDL_CONTROLLER_AXIS_LEFTY);
      if (x < -16000) pad[p][RETRO_DEVICE_ID_JOYPAD_LEFT] = 1;
      if (x > 16000) pad[p][RETRO_DEVICE_ID_JOYPAD_RIGHT] = 1;
      if (y < -16000) pad[p][RETRO_DEVICE_ID_JOYPAD_UP] = 1;
      if (y > 16000) pad[p][RETRO_DEVICE_ID_JOYPAD_DOWN] = 1;
    }
  }
}

#endif

/* The core's own virtual keyboard (Select shows/hides it, touching a key presses it).
   Without a gamepad Select is generated by tapping the top-right corner (or Alt+K). */
static int last_key_char = 0;
static Uint32 last_key_tick = 0;
static int select_pulse = 0, select_active = 0, corner_touch = 0;

static int in_vkb_corner(int x, int y)
{
  int w = 0, h = 0;
#ifdef THEODORE_SDL12
  if (screen != NULL) { w = screen->w; h = screen->h; }
#else
  SDL_GetWindowSize(window, &w, &h);
#endif
  return w > 0 && h > 0 && x > w * 85 / 100 && y < h * 15 / 100;
}

static int in_paste_corner(int x, int y)
{
  int w = 0, h = 0;
#ifdef THEODORE_SDL12
  if (screen != NULL) { w = screen->w; h = screen->h; }
#else
  SDL_GetWindowSize(window, &w, &h);
#endif
  return w > 0 && h > 0 && x < w * 15 / 100 && y < h * 15 / 100;
}

static void update_pointer(int x, int y)
{
  if (dest.w <= 0 || dest.h <= 0) return;
  mouse_x = (int) (((double) (x - dest.x) / dest.w) * 65535.0 - 32767.0);
  mouse_y = (int) (((double) (y - dest.y) / dest.h) * 65535.0 - 32767.0);
  if (mouse_x < -32767) mouse_x = -32767;
  if (mouse_x > 32767) mouse_x = 32767;
  if (mouse_y < -32767) mouse_y = -32767;
  if (mouse_y > 32767) mouse_y = 32767;
}

/* Left button / touch: the top-right corner toggles the core's virtual keyboard, the rest is the light pen */
static void pointer_button(int x, int y, int down)
{
  if (down && in_vkb_corner(x, y)) { corner_touch = 1; select_pulse = 3; return; }
  if (down && kbd_icon && in_paste_corner(x, y)) { corner_touch = 1; app_paste(); return; }
  if (!down && corner_touch) { corner_touch = 0; return; }
  update_pointer(x, y);
  mouse_down = down;
}

static void toggle_fullscreen(void)
{
#ifdef THEODORE_SDL12
  if (screen != NULL) SDL_WM_ToggleFullScreen(screen);
#else
  Uint32 f = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_SetWindowFullscreen(window, f ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif
}

static int mod_swallow = 0;   /* a paste hotkey is held: its modifier must not reach the Thomson keyboard */

static int hotkey(SDL_Keycode k)
{
  static int shots = 0;
  char name[64];
  switch (k)
  {
    case SDLK_RETURN: toggle_fullscreen(); return 1;
    case SDLK_p:
      if (core.theodore_dbg_is_halted()) core.theodore_dbg_resume(); else core.theodore_dbg_halt();
      return 1;
    case SDLK_s:
      dbg_exec("step");
      return 1;
    case SDLK_r: app_reset(); return 1;
    case SDLK_c:
      snprintf(name, sizeof(name), "shot_%04d.bmp", shots++);
      printf("screenshot %s: %s\n", name, app_screenshot(name) == 0 ? "ok" : "failed");
      return 1;
    case SDLK_f: fast_forward = !fast_forward; return 1;
    case SDLK_1: printf("state saved: %s\n", app_save_state("state0.sav") == 0 ? "state0.sav" : "failed"); return 1;
    case SDLK_2: pending_load_state = 1; return 1;
    case SDLK_q: quit_flag = 1; return 1;
    case SDLK_k: select_pulse = 3; return 1;   /* show/hide the core's virtual keyboard */
    case SDLK_v: app_paste(); mod_swallow = 1; return 1;
    case SDLK_x: app_type_stop(); printf("typing stopped\n"); return 1;
    default: return 0;
  }
}

static void handle_events(void)
{
  SDL_Event e;
  while (SDL_PollEvent(&e))
  {
    switch (e.type)
    {
      case SDL_QUIT: quit_flag = 1; break;
#ifdef THEODORE_SDL12
      case SDL_VIDEORESIZE:
        screen = SDL_SetVideoMode(e.resize.w, e.resize.h, 16, SDL_SWSURFACE | SDL_RESIZABLE);
        break;
#else
      case SDL_CONTROLLERDEVICEADDED: open_controllers(); break;
#endif
      case SDL_MOUSEMOTION:
        if (!TOUCH_MOUSE(e.motion.which)) update_pointer(e.motion.x, e.motion.y);
        break;
      case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        if (TOUCH_MOUSE(e.button.which)) break;   /* touches are handled through the finger events */
        if (e.button.button == SDL_BUTTON_LEFT) pointer_button(e.button.x, e.button.y, e.type == SDL_MOUSEBUTTONDOWN);
        else update_pointer(e.button.x, e.button.y);
        break;
#ifndef THEODORE_SDL12
      case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
      {
        int ww = 0, wh = 0, fx, fy;
        SDL_GetWindowSize(window, &ww, &wh);
        fx = (int) (e.tfinger.x * ww); fy = (int) (e.tfinger.y * wh);
        if (e.type == SDL_FINGERMOTION) update_pointer(fx, fy);
        else pointer_button(fx, fy, e.type == SDL_FINGERDOWN);
        break;
      }
      case SDL_TEXTINPUT:
        /* printable characters from an on-screen keyboard (see KEYDOWN below for the rest) */
        if (opt.text_input)
        {
          int dup = 0;
          if (e.text.text[0] != '\0' && e.text.text[1] == '\0' && last_key_char != 0
              && SDL_GetTicks() - last_key_tick < 200)
          {
            int c = (unsigned char) e.text.text[0];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            dup = (c == last_key_char);
          }
          if (!dup) app_type(e.text.text);
          last_key_char = 0;
        }
        break;
#endif
      case SDL_KEYDOWN: case SDL_KEYUP:
      {
        int down = e.type == SDL_KEYDOWN;
        SDL_Keycode k = e.key.keysym.sym;
        if (getenv("THEODORE_KEYLOG") != NULL)
#ifdef THEODORE_SDL12
          printf("key %s sym=%d unicode=%d mod=%d\n", down ? "down" : "up", (int) k, (int) e.key.keysym.unicode, (int) e.key.keysym.mod);
#else
          printf("key %s sym=%d scancode=%d mod=%d repeat=%d\n", down ? "down" : "up", (int) k, (int) e.key.keysym.scancode, (int) e.key.keysym.mod, (int) e.key.repeat);
        if (e.key.repeat) break;
#endif
        if (down && (e.key.keysym.mod & KMOD_ALT) && hotkey(k)) break;
        if (down && (((e.key.keysym.mod & KMOD_CTRL) && k == SDLK_v) || ((e.key.keysym.mod & KMOD_SHIFT) && k == SDLK_INSERT)))
        {
          /* release the modifiers the core may already have seen: typed characters must not be combined with them */
          send_key(RETROK_LCTRL, 0); send_key(RETROK_RCTRL, 0); send_key(RETROK_LSHIFT, 0); send_key(RETROK_RSHIFT, 0);
          mod_swallow = 1;
          app_paste();
          break;
        }
        if (!down && (k == SDLK_v || k == SDLK_INSERT) && mod_swallow) break;
        if (mod_swallow && (k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LALT || k == SDLK_RALT))
        {
          if (!down) mod_swallow = 0;   /* the user let go of the modifier: back to normal */
          break;
        }
        /* remember printable key presses: the same character may follow as SDL_TEXTINPUT (typed once only) */
        if (down && k >= 32 && k < 127) { last_key_char = (int) k; last_key_tick = SDL_GetTicks(); }
        if (opt.keyjoy && keyjoy_key(k, down)) break;
#ifdef THEODORE_SDL12
        {
          unsigned rk = sdl_to_retrok(k);
          int u = e.key.keysym.unicode;
          if (rk == 0 && down && u >= 32 && u < 127)   /* overlay keyboards may send a character without a usable keysym */
          {
            char t[2]; t[0] = (char) u; t[1] = '\0';
            app_type(t);
          }
          else send_key(rk, down);
        }
#else
        send_key(opt.phys_keys ? scancode_to_retrok(e.key.keysym.scancode) : sdl_to_retrok(k), down);
#endif
        break;
      }
      default: break;
    }
  }
}

/* -------------------------------------------------------------------- main */

/* Errors must be visible where there is no console (Android): message box + stderr. */
static void report_error(const char *fmt, ...)
{
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  fprintf(stderr, "%s", msg);
#ifndef THEODORE_SDL12
  if (strcmp(SDL_GetPlatform(), "Android") == 0) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Theodore", msg, NULL);
#endif
}

static int parse_args(int argc, char **argv)
{
  int i;
  memset(&opt, 0, sizeof(opt));
#ifdef _WIN32
  opt.core_path = "theodore_libretro.dll";
#else
  opt.core_path = "./theodore_libretro.so";
#endif
  opt.model = "PC128";
  opt.scale = 1.5;
  opt.keyjoy = 1;
  for (i = 1; i < argc; i++)
  {
    const char *a = argv[i];
    const char *next = i + 1 < argc ? argv[i + 1] : NULL;
#define NEEDARG() do { if (next == NULL) { report_error( "%s needs an argument\n", a); return -1; } i++; } while (0)
    if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 1; }
    else if (!strcmp(a, "--core")) { NEEDARG(); opt.core_path = next; }
    else if (!strcmp(a, "--model")) { NEEDARG(); opt.model = next; }
    else if (!strcmp(a, "--scale")) { NEEDARG(); opt.scale = atof(next); }
    else if (!strcmp(a, "--set")) { NEEDARG(); if (opt.nsets < 32) opt.sets[opt.nsets++] = next; }
    else if (!strcmp(a, "--script")) { NEEDARG(); opt.script = next; }
    else if (!strcmp(a, "--frames")) { NEEDARG(); opt.max_frames = atol(next); }
    else if (!strcmp(a, "--screenshot")) { NEEDARG(); opt.screenshot = next; }
    else if (!strcmp(a, "--fullscreen")) opt.fullscreen = 1;
    else if (!strcmp(a, "--nearest")) opt.nearest = 1;
    else if (!strcmp(a, "--no-audio")) opt.no_audio = 1;
    else if (!strcmp(a, "--no-console")) opt.no_console = 1;
    else if (!strcmp(a, "--turbo")) opt.turbo = 1;
    else if (!strcmp(a, "--no-keyjoy")) opt.keyjoy = 0;
    else if (!strcmp(a, "--phys-keys")) opt.phys_keys = 1;
    else if (!strcmp(a, "--text-input")) opt.text_input = 1;
    else if (!strcmp(a, "--kbd-icon")) kbd_icon = 1;
    else if (!strcmp(a, "--log")) { NEEDARG(); if (freopen(next, "w", stdout) != NULL) { dup2(fileno(stdout), fileno(stderr)); setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); } }
    else if (!strcmp(a, "--debug-port")) { NEEDARG(); opt.debug_port = next; }
    else if (!strcmp(a, "--halt")) opt.start_halted = 1;
    else if (!strncmp(a, "--bridge-", 9))
    {
      char buf[512];
      NEEDARG();
      if (opt.nbridges >= 8) { report_error( "too many bridges\n"); return -1; }
      snprintf(buf, sizeof(buf), "%s|%s", a + 9, next);
      opt.bridges[opt.nbridges++] = strdup(buf);
    }
    else if (a[0] == '-') { report_error( "unknown option %s (try --help)\n", a); return -1; }
    else opt.content = a;
#undef NEEDARG
  }
  return 0;
}

/* Options from a file (no way to pass arguments on Android): theodore.args in the current directory or
   /sdcard/theodore.args, whitespace separated, e.g.  --debug-port 5555 --model PC128 disk.fd
   They come before the real command-line arguments. */
static char **args_from_file(int *argc, char **argv)
{
  static const char *paths[] = { "theodore.args", "/sdcard/theodore.args" };
  size_t k;
  for (k = 0; k < sizeof(paths) / sizeof(paths[0]); k++)
  {
    FILE *f = fopen(paths[k], "rb");
    char *buf, *tok;
    char **out;
    long n;
    int cnt = 0, cap, i;
    if (f == NULL) continue;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 65536 || (buf = (char *) malloc((size_t) n + 1)) == NULL) { fclose(f); continue; }
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) { fclose(f); free(buf); continue; }
    fclose(f);
    buf[n] = '\0';
    cap = *argc + (int) n / 2 + 2;
    out = (char **) malloc((size_t) cap * sizeof(char *));
    out[cnt++] = argv[0];
    for (tok = strtok(buf, " \t\r\n"); tok != NULL && cnt < cap - 1; tok = strtok(NULL, " \t\r\n")) out[cnt++] = tok;
    for (i = 1; i < *argc && cnt < cap - 1; i++) out[cnt++] = argv[i];
    out[cnt] = NULL;
    printf("options read from %s\n", paths[k]);
    *argc = cnt;
    return out;
  }
  return argv;
}

int main(int argc, char **argv)
{
  char err[256];
  struct retro_system_info si;
  int i, rc;
  Uint64 freq, next_frame, now;
  double frame_ticks;
  Uint32 title_time = 0;
  int frames_this_sec = 0;
  double fps_shown = 0.0;

#if defined(THEODORE_SDL12) && defined(THEODORE_ANDROID)
  if (freopen("/sdcard/theodore.log", "w", stdout) != NULL)   /* SDL 1.2: no app storage API, no console */
  {
    dup2(fileno(stdout), fileno(stderr));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
  }
#elif defined(__ANDROID__) || defined(THEODORE_ANDROID)
  {
    /* no console on Android: keep a log in the app's external storage directory */
    const char *dir = SDL_AndroidGetExternalStoragePath();
    char path[512];
    if (dir != NULL)
    {
      snprintf(path, sizeof(path), "%s/theodore.log", dir);
      if (freopen(path, "w", stdout) != NULL)
      {
        dup2(fileno(stdout), fileno(stderr));
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        printf("log file: %s\n", path);
      }
    }
    printf("argc=%d", argc);
    for (i = 0; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\n");
  }
#endif
  argv = args_from_file(&argc, argv);
  rc = parse_args(argc, argv);
  if (rc != 0) return rc > 0 ? 0 : 2;

#ifdef THEODORE_SDL12
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
#else
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
#endif
  {
    report_error( "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  if (core_load(opt.core_path, err, sizeof(err)) != 0)
  {
    report_error( "%s\n", err);
    return 1;
  }

  core.retro_set_environment(env_cb);
  core.retro_set_video_refresh(video_cb);
  core.retro_set_audio_sample(audio_cb);
  core.retro_set_audio_sample_batch(audio_batch_cb);
  core.retro_set_input_poll(poll_cb);
  core.retro_set_input_state(input_cb);
  core.retro_init();

  app_set_var("theodore_rom", opt.model);
  for (i = 0; i < opt.nsets; i++)
  {
    char key[64], *eq;
    snprintf(key, sizeof(key), "%s", opt.sets[i]);
    eq = strchr(key, '=');
    if (eq == NULL) { report_error( "--set expects KEY=VALUE, got '%s'\n", opt.sets[i]); return 2; }
    *eq = '\0';
    app_set_var(key, eq + 1);
  }
  vars_updated = 0;

  memset(&si, 0, sizeof(si));
  core.retro_get_system_info(&si);
  printf("core: %s %s, model %s\n", si.library_name, si.library_version, app_model());

  if (opt.content != NULL)
  {
    struct retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.path = opt.content;
    if (!core.retro_load_game(&gi)) { report_error( "the core refused '%s'\n", opt.content); return 1; }
  }
  else if (!core.retro_load_game(NULL))
  {
    report_error( "retro_load_game failed\n");
    return 1;
  }
  memset(&av, 0, sizeof(av));
  core.retro_get_system_av_info(&av);
  if (av.timing.fps <= 0) av.timing.fps = 50.0;

#ifdef THEODORE_SDL12
  {
    int w = (int) (av.geometry.base_width * opt.scale);
    int h = (int) (av.geometry.base_width * opt.scale / av.geometry.aspect_ratio);
    screen = SDL_SetVideoMode(w, h, 16, SDL_SWSURFACE | SDL_RESIZABLE | (opt.fullscreen ? SDL_FULLSCREEN : 0));
    if (screen == NULL) { report_error("SDL_SetVideoMode: %s\n", SDL_GetError()); return 1; }
    SDL_WM_SetCaption("Theodore Debug Frontend", NULL);
    SDL_EnableUNICODE(1);   /* characters from overlay / on-screen keyboards arrive in keysym.unicode */
    if (getenv("THEODORE_TOUCH") != NULL) kbd_icon = 1;
  }
  if (!opt.no_audio && !opt.turbo && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
  {
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = (int) av.timing.sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_fill;
    if (SDL_OpenAudio(&want, NULL) == 0) { audio_dev = 1; SDL_PauseAudio(0); }
    else fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
  }
#else
  window = SDL_CreateWindow("Theodore Debug Frontend", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            (int) (av.geometry.base_width * opt.scale),
                            (int) (av.geometry.base_width * opt.scale / av.geometry.aspect_ratio),
                            SDL_WINDOW_RESIZABLE | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  if (window == NULL) { report_error( "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  /* recognised at run time: the compiler used on Android may not define __ANDROID__ */
  if (strcmp(SDL_GetPlatform(), "Android") == 0 || getenv("THEODORE_TOUCH") != NULL)
  {
    opt.text_input = 1;
    kbd_icon = 1;
  }
  if (opt.text_input)
  {
    SDL_Rect r;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    r.x = 0; r.y = wh * 2 / 3; r.w = ww > 0 ? ww : 1; r.h = wh / 3 > 0 ? wh / 3 : 1;
    SDL_SetTextInputRect(&r);   /* some Android builds show no keyboard without a non-empty rect */
    SDL_StartTextInput();
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, opt.nearest ? "0" : "1");
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == NULL) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (renderer == NULL) { report_error( "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                              (int) av.geometry.max_width, (int) av.geometry.max_height);
  if (texture == NULL) { report_error( "SDL_CreateTexture: %s\n", SDL_GetError()); return 1; }

  if (!opt.no_audio && !opt.turbo && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
  {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int) av.timing.sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev != 0) SDL_PauseAudioDevice(audio_dev, 0);
    else fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
  }
#endif
  open_controllers();

  /* external hardware */
  for (i = 0; i < opt.nbridges; i++)
  {
    if (bridge_attach_spec(opt.bridges[i], err, sizeof(err)) != 0)
    {
      report_error( "bridge '%s': %s\n", opt.bridges[i], err);
      return 1;
    }
  }

  dbg_init();
  if (opt.script != NULL && dbg_script_load(opt.script) != 0) return 1;
  if (opt.debug_port != NULL && dbg_remote_start(opt.debug_port) != 0) return 1;
  if (!opt.no_console) dbg_console_start();
  if (opt.start_halted) core.theodore_dbg_halt();

  freq = SDL_GetPerformanceFrequency();
  frame_ticks = (double) freq / av.timing.fps;
  next_frame = SDL_GetPerformanceCounter();
  title_time = SDL_GetTicks();

  while (!quit_flag)
  {
    handle_events();
    poll_controllers();
    if (select_pulse > 0) { pad[0][RETRO_DEVICE_ID_JOYPAD_SELECT] = 1; select_pulse--; select_active = 1; }
    else if (select_active) { pad[0][RETRO_DEVICE_ID_JOYPAD_SELECT] = 0; select_active = 0; }
    if (!opt.keyjoy) { /* nothing */ }
    if (pending_load_state)
    {
      printf("state loaded: %s\n", app_load_state("state0.sav") == 0 ? "ok" : "failed");
      pending_load_state = 0;
    }
    dbg_poll();
    type_pump();

    core.retro_run();
    frame_count++;
    frames_this_sec++;

    dbg_check_stop();

    present();

    if (opt.max_frames > 0 && (long) frame_count >= opt.max_frames && !dbg_busy()) break;

    now = SDL_GetPerformanceCounter();
    if (SDL_GetTicks() - title_time >= 500)
    {
      char title[160];
      fps_shown = frames_this_sec * 1000.0 / (SDL_GetTicks() - title_time);
      frames_this_sec = 0;
      title_time = SDL_GetTicks();
      snprintf(title, sizeof(title), "Theodore Debug - %s - %.1f fps - %s%s", app_model(), fps_shown,
               core.theodore_dbg_is_halted() ? "HALTED" : "running", fast_forward ? " (fast)" : "");
#ifdef THEODORE_SDL12
      SDL_WM_SetCaption(title, NULL);
#else
      SDL_SetWindowTitle(window, title);
#endif
    }
    if (!opt.turbo && !fast_forward)
    {
      next_frame += (Uint64) frame_ticks;
      if (now > next_frame + (Uint64) (frame_ticks * 5)) next_frame = now; /* far behind: resync */
      while ((now = SDL_GetPerformanceCounter()) < next_frame)
      {
        Uint64 left = next_frame - now;
        if (left > freq / 500) SDL_Delay(1);
      }
    }
    else next_frame = SDL_GetPerformanceCounter();
  }

  if (opt.screenshot != NULL)
  {
    printf("screenshot %s: %s\n", opt.screenshot, app_screenshot(opt.screenshot) == 0 ? "ok" : "failed");
  }
  bridge_shutdown();
  core.retro_unload_game();
  core.retro_deinit();
  free(framebuf);
#ifdef THEODORE_SDL12
  if (audio_dev != 0) SDL_CloseAudio();
  if (scaled != NULL) SDL_FreeSurface(scaled);
#else
  if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
#endif
  SDL_Quit();
  return 0;
}
