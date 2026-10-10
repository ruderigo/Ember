# Ember - build on the handheld itself:  make
# (needs gcc, libsdl2-dev, libasound2-dev; ArkOS4Clone already has them)
CC      ?= gcc
ARCH    := $(shell uname -m)
VERSION := $(shell cat VERSION 2>/dev/null || echo dev)
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -DEMBER_VERSION='"$(VERSION)"' $(EXTRA_CFLAGS)
ifeq ($(ARCH),aarch64)
CFLAGS  += -mcpu=cortex-a35 -ffast-math
else
CFLAGS  += -ffast-math
endif
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2 -D_REENTRANT)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null || echo -lSDL2)
LIBS    := $(SDL_LIBS) -lasound -lpthread -lm

SRC := src/main.c src/dsp.c src/io.c src/rec.c

ember: $(SRC) src/dsp.h src/io.h src/rec.h src/font.h VERSION
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC) -o $@ $(LIBS)

# offline test: renders a chord progression to ember_demo.wav and reports speed
render: tools/render.c src/dsp.c src/dsp.h
	$(CC) $(CFLAGS) tools/render.c src/dsp.c -o $@ -lm

looptest: tools/looptest.c src/dsp.c src/dsp.h
	$(CC) $(CFLAGS) tools/looptest.c src/dsp.c -o $@ -lm

rectest: tools/rectest.c src/rec.c src/rec.h
	$(CC) $(CFLAGS) tools/rectest.c src/rec.c -o $@ -lm -lpthread

# all tests exit non-zero on failure (used by CI)
test: render looptest rectest
	./render ember_demo.wav
	./looptest
	./rectest

# ready-to-install packages in dist/ (see tools/package.sh)
package: ember
	tools/package.sh

clean:
	rm -rf ember render looptest rectest ember_demo.wav looptest.wav dist recordings

.PHONY: test package clean
