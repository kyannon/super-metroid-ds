# Super Metroid DS build
#
#   scripts/setup-host.sh     first time: fetches snes9x and SDL2 into deps/, builds everything
#   make                      host build: build/host/sm-ds and the cores build/host/sm-core-{a,b}.so
#   make tools                build/host/probe (scripted core runs, PNG shots, states)
#   make CROSS=aarch64-linux-gnu- SDL_PREFIX=...   cross build
#
# SNES9X      snes9x source tree (its libretro core is built from it)
# SDL_PREFIX  an SDL2 install prefix (include/SDL2, lib/libSDL2.so)

CROSS      ?=
CC         := $(CROSS)gcc
CXX        := $(CROSS)g++
SNES9X     ?= deps/snes9x
SDL_PREFIX ?= deps/sdl-host
OUT        ?= build/$(if $(CROSS),$(patsubst %-,%,$(CROSS)),host)

VERSION    := $(shell cat VERSION 2>/dev/null || echo dev)
OPTFLAGS   ?= -O2 -g
override CFLAGS := $(OPTFLAGS) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
              -DSMDS_VERSION='"$(VERSION)"' -I$(SDL_PREFIX)/include/SDL2 -D_REENTRANT
LDFLAGS    += -L$(SDL_PREFIX)/lib -Wl,-rpath-link,$(SDL_PREFIX)/lib -Wl,-rpath,'$$ORIGIN'
# the desktop build finds deps/ SDL2 where it is (the device uses its own)
ifeq ($(CROSS),)
LDFLAGS    += -Wl,-rpath,$(abspath $(SDL_PREFIX))/lib
endif
LIBS       := -lSDL2 -lpthread -lm -ldl

CORE_SRC   := src/emu.c src/mapcap.c src/sm.c
APP_SRC    := src/main.c src/config.c src/font.c src/ui.c src/game.c src/rewind.c src/cheats.c src/ra.c src/paths.c src/image.c src/textfont.c src/fonts_data.c $(CORE_SRC)

all: $(OUT)/sm-ds cores

$(OUT)/sm-ds: $(APP_SRC) src/*.h VERSION
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(APP_SRC) $(LDFLAGS) $(LIBS)

cores:
	$(MAKE) -C core SNES9X=$(abspath $(SNES9X)) OUT=$(abspath $(OUT)) CXX=$(CXX) CC=$(CC)

tools: $(OUT)/probe

$(OUT)/probe: tools/probe.c
	@mkdir -p $(OUT)
	$(CC) -O2 -w -o $@ tools/probe.c -ldl

clean:
	rm -rf build

.PHONY: all cores tools clean

# the regression tests (tests/run.py; needs tests/fixtures/, see tests/README.md)
test: all
	python3 tests/run.py --no-build
.PHONY: test
