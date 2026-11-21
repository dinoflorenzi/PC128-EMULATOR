# Theodore debug frontend

SDL2 frontend for a patched [Theodore](https://github.com/Zlika/theodore) libretro core
(Thomson TO7…TO9+, MO5, MO6, Olivetti Prodest PC128). It adds what is needed to debug
6809 software and hardware attached to the machine:

* non-blocking debugger: PC / read / write breakpoints, **data-pattern breakpoints**,
  trace ring (8192 instructions), halt/step/resume, side-effect-free memory view and disassembler;
* attachment points for external hardware: **expansion bus**, **cartridge connector**,
  **joystick ports through a Motorola 6821 PIA**, each served by a callback in the core
  or by an external device over a TCP bridge (e.g. a microcontroller, see `BRIDGE_PROTOCOL.md`).

License: GPL-3 (same as Theodore).

## Build

Needs SDL2 development files.

    # core (from the repository root). The core Makefile does not track header
    # dependencies: always clean first after pulling/changing any .h
    make clean && make DASM=1 -j4          # -> theodore_libretro.so

    # frontend
    cd frontend && make                    # -> theodore-fe

Linux: `apt install libsdl2-dev`. Windows: MSYS2 MINGW64 shell,
`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-SDL2 make unzip`.
Build the core with **`make platform=win DASM=1 -j4`** (automatic platform detection may pick unix
and not produce the .dll), then `make` in `frontend/` and copy `SDL2.dll` next to `theodore-fe.exe`.
Confirmed working on Windows by the user (MSYS2 MINGW64).

**Single executable:** after building the core, `make clean && make STATIC=1` in `frontend/` links the core
into `theodore-fe` (no `.dll/.so` needed, `--core` is ignored). The core objects are reused as the core
Makefile produced them, so build the core first with the same `platform=`. Without `STATIC=1` the
core is loaded at run time (`--core`), so it can be rebuilt without relinking the frontend.

**SDL 1.2 build** (needed for C4droid's overlay keyboard / d-pad on Android, which it draws only for SDL 1.2 programs):

    make clean && make DASM=1 platform=unix -j4        # core, in the repository root
    make -C frontend clean && make -C frontend SDL12=1 OUT=../pc128_sdl12

`SDL12=1` uses `sdl-config`, implies `STATIC=1` (SDL 1.2 has no `SDL_LoadObject`) and defines `THEODORE_SDL12`.
`SHARED=1` can be added when the launcher wants a shared object. Differences from the SDL2 build: software
scaling of the frame (nearest neighbour) into `SDL_SetVideoMode`, audio through an `SDL_OpenAudio` callback,
generic joystick API (hat/stick = directions, button 0/1 = fire, 6/7 = select/start), keys by character only
(`--phys-keys` ignored), a character that arrives with no usable keysym is typed from `keysym.unicode`.
`THEODORE_KEYLOG=1` prints every key event (sym/unicode/mod): useful to see what an overlay keyboard really sends.
`--log FILE` writes stdout/stderr to FILE; on Android SDL 1.2 builds the log goes to `/sdcard/theodore.log`.
Verified on Linux only (headless frame identical to the SDL2 build; keys and scaling under Xvfb with sdl12-compat);
**not tested on Android**.

## Debugger on Android (no console)

`--debug-port [ADDR:]PORT` serves the debugger commands over TCP (default address 127.0.0.1 = this device only;
`0.0.0.0:5555` lets a PC on the same Wi-Fi connect: there is **no authentication**, use it on a trusted network).
All debugger output is sent to the client (and still to the log). One client at a time, reconnecting is fine.
C4droid cannot pass arguments: put them in a file `theodore.args` (whitespace separated, in the program's
directory or `/sdcard/theodore.args`), e.g. `--debug-port 5555`. Clients: `nc 127.0.0.1 5555` (Termux),
`tools/dbgnc.c` (compile it with C4droid as a console program, run it in a floating/split window), or from a PC
`nc PHONE_IP 5555`. Not available on Windows (use the console there).

## Running

    ./theodore-fe --core ../theodore_libretro.so --model PC128 [content]

Content: `.k7 .fd .sap .rom .m5 .m7`. `--set KEY=VALUE` sets any core variable
(e.g. `theodore_autorun=enabled`). Hotkeys, with Alt held: Enter fullscreen, P halt/resume,
S step, R reset, C screenshot, F fast-forward, 1/2 save/load state, Q quit.
The numeric keypad is joystick 0 (`--no-keyjoy` disables it); the mouse is the light pen.

Commands are typed on stdin (or `--script FILE`; `--no-console` turns stdin off).
`help` lists them. Headless use: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` with
`--frames N --screenshot out.bmp`.

### Debugger examples (addresses in hex)

    bp pc C000                  stop before the instruction at C000
    bp w A7E0                   stop after a write to A7E0
    bpd w A7C0-A7FF 12,34       stop when 12 then 34 are written consecutively in that range
    bpd rw 0000-9FFF 80 C0      masked: (byte & C0) == (80 & C0)
    trace on / halt / trace dump 20
    mem A7C0 40 / dis C000 10   no side effects on the emulated hardware
    find 0000-FFFF "GAME OVER"  search text (-i ignores case); only the CPU-visible memory
    find 2000-9FFF 86,A5,??,B7  search bytes, ?? = any byte; up to 64 bytes, 100 results per command

A data breakpoint compares the last N *accessed* bytes (N = pattern length, up to 4) inside
the range and access type; it stops after the access that completes the pattern.

## Paste and typing

`Ctrl+V`, `Shift+Insert` or `Alt+V` type the clipboard on the Thomson keyboard (SDL2). On Android tap the
top-left corner (it needs `--kbd-icon`, on by default there). Debugger: `paste`, `typefile FILE` (any text file),
`type TEXT`, `typestop` (drop what is still queued, also Alt+X), `typespeed HOLD GAP AFTER_RETURN` (frames).
SDL 1.2 has no clipboard API: `paste` reads `paste.txt` (current directory or `/sdcard/paste.txt`); `typefile` works everywhere.

* Text is fed to the keyboard a few characters at a time, so any length is fine. CRLF/CR become one Return, tabs
  become spaces, typographic quotes/dashes become ASCII, other non-ASCII characters are skipped and reported.
* On **PC128 and MO6** each character is typed by its real key position (Thomson matrix scancode + Shift), measured on the
  emulator itself by `tools/calibrate_typemap.py` (result in `typemap.h`): 91 of the 94 printable characters can be typed.
  **PC128 cannot type `\ | ~`; MO6 cannot type `` ` | ~``**: no key produces them in BASIC 128. The MO6 keyboard is
  AZERTY, so the older libretro key mapping typed wrong letters there; this fixes `type` too.
* On the other models the libretro key codes are used: they follow the position of the key, so only letters, digits and
  space come out right.
* Letters: Caps Lock is on after reset and BASIC then gives capitals for both plain and Shift keys, so lower case text
  types as capitals. Press the Caps Lock key first to keep the case (plain key = lower, Shift = upper; verified on PC128).
* The physical Ctrl/Shift used for the shortcut is not forwarded to the Thomson while the text is typed.
* Speed: default `typespeed 3 1 30` (key held 3 frames, released 1, then 30 frames after Return, about 12 characters/s).
  The pause after Return is needed: BASIC stores the line and, once the screen is full, scrolls it, and keys pressed meanwhile
  are lost (the first character of the next line). Measured on the PC128 with 60-line listings, checked in RAM against the
  source text: pause 18 and 25 correct; 12 loses lines; a key held for 1 frame loses keys. Defaults keep a margin.
  `Alt+F` (fast forward) shortens the wait.
* `press CODE [sbac]` (libretro key code, decimal) and `scan HEX [sbac]` (matrix position 00-4F) press one raw key,
  with Shift / BASIC / RAZ / CNT; `scan` reaches keys that have no libretro code.

## Hardware slots

| option | slot | window |
|---|---|---|
| `--bridge-exp HOST:PORT@BASE+SIZE[,irq=N][,poll=C][,override]` | expansion bus | inside A000-A7FF (addresses the core handles natively are refused unless `override`) |
| `--bridge-cart HOST:PORT@BASE+SIZE[,irq=N][,poll=C][,ctl]` | cartridge | inside B000-EFFF; `ctl` also claims A7CB (bypasses the built-in banking) |
| `--bridge-joy HOST:PORT[,irq=N][,poll=C][,builtin][,std]` | joystick ports | 6821 at A7CC-A7CF (Thomson layout; `std` = standard RS layout); `builtin` keeps pads/keypad wired-AND with the external pins |

`slots`, `irq`, `pia` show the state; `pia pins A|B VAL` and `pia line ca1|ca2|cb1|cb2 0|1`
drive the PIA inputs by hand.

The 6821 model covers DDR/OR/CR, CA1/CB1 edge detection, CA2/CB2 as manual output,
read-strobe handshake and pulse, flag clearing on data read. Wiring of the PIA IRQ
line to the CPU is **not documented in the core**, so it is off unless you pass `irq=N`.

### Test device

`tools/fake_micro.py` simulates a device (`exp` or `joy`); `tools/test_bridge.py` runs
the scenarios (round trip, cycle stamps equal to the emulator trace, IRQ, reset,
link loss, stall). Both need only Python 3.

## Limits

* Bridge is lock-step TCP: one round trip per access, so heavy use runs slower than real time.
  A 500 ms answer timeout marks the device DOWN and emulation goes on without it.
* Save states cover the machine only, **not** the state of external devices.
* `type` maps characters to Thomson keys; characters with no key are counted and reported.
* A cartridge slot with `ctl` replaces the core's cartridge banking while attached.
* The native (PIA-less) joystick path and the PIA model differ in one case: with DDRB bits 0-5
  as inputs the PIA reads 1, the native path returns the DAC level. With DDRB=3F both agree.
* Not tested: real serial hardware, real media beyond the built-in ROM boot.
* On-screen keyboards (Android): the frontend accepts both key events and SDL text input (a character that
  arrives twice, as key and as text, is typed once; `--text-input`, default on Android). The core's own Thomson
  virtual keyboard can be shown without a gamepad by tapping the top-right corner of the window (a small keyboard
  button is drawn there; `--kbd-icon`, default on Android; Alt+K on a PC). Touches are handled as finger events;
  touching a key presses it. Symbols without a key in the core's key map (e.g. `+` on some models) are ignored
  in text mode. On Android errors are shown in a message box and logged to `theodore.log` in the app's external
  storage directory. None of this has been tested on a real device.
