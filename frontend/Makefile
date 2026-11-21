# Theodore debug frontend. Needs SDL2 (development files), or SDL 1.2 with SDL12=1.
#   Linux:           make            (apt install libsdl2-dev)
#   Windows (MSYS2): make            (pacman -S mingw-w64-x86_64-SDL2, run from the MINGW64 shell)
CC      ?= gcc
.DEFAULT_GOAL := all
CORE    := ..
# SDL12=1: build against SDL 1.2 (sdl-config). The core is always linked in (STATIC=1 implied).
# C4droid shows its overlay (Kbd button, d-pad) only for SDL 1.2 programs.
ifeq ($(SDL12),1)
  SDLCFG ?= sdl-config
  STATIC := 1
endif
SDLCFG  ?= sdl2-config
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=gnu99 -I$(CORE)/src -I$(CORE)/src/libretro-common/include $(shell $(SDLCFG) --cflags)
# Android (C4droid's gcc may not define __ANDROID__): enables the log file in the app's storage directory
ifneq (,$(findstring android,$(shell $(CC) -dumpmachine 2>/dev/null)))
  CFLAGS += -DTHEODORE_ANDROID
endif
# -mwindows (added by sdl2-config on Windows) would detach stdin/stdout: the debugger needs a console
LDLIBS  += $(filter-out -mwindows,$(shell $(SDLCFG) --libs))

ifeq ($(OS),Windows_NT)
  EXE   := theodore-fe.exe
  LDLIBS += -lws2_32
else
  EXE   := theodore-fe
endif

SRC := main.c core.c dbgcli.c bridge.c
OBJ := $(SRC:.c=.o)

# STATIC=1: the core is linked into the executable (no theodore_libretro.dll/.so to ship).
# Build the core first (make DASM=1 [platform=win] in the repository root): its .o files are reused.
ifeq ($(STATIC),1)
  CORE_DIR := $(CORE)
  SOURCES_C :=
  include $(CORE)/Makefile.common
  CORE_OBJ := $(SOURCES_C:.c=.o)
  CFLAGS += -DCORE_STATIC
  LDLIBS += -lm
  OBJ += $(CORE_OBJ)
# never recompile the core objects with the frontend's flags (the core needs its own, e.g. -fsigned-char)
$(CORE_OBJ):
	@test -f $@ || { echo "$@ missing: build the core first (make DASM=1 in $(CORE))"; false; }
endif

# SHARED=1 [OUT=file]: build a shared object exporting SDL_main instead of an executable. This is
# what SDL2 on Android (e.g. the "SDL2 application" run mode of C4droid) loads inside its Activity.
# Untested on a real Android device. -pie cannot be combined with -shared, so it is removed.
OUT ?= $(EXE)
ifeq ($(SHARED),1)
  CFLAGS  := $(filter-out -pie,$(CFLAGS)) -fPIC
  LDFLAGS := $(filter-out -pie,$(LDFLAGS)) -shared
endif

all: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

# header changes must rebuild everything (the core Makefile does not track them either)
%.o: %.c $(wildcard *.h) $(wildcard $(CORE)/src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(EXE) $(OUT)

.PHONY: all clean
