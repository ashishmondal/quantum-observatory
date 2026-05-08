# Adafruit GFX TTF → GFXfont header converter.
#
# fontconvert.c is vendored verbatim from the Adafruit_GFX library
# (.pio/libdeps/pico/Adafruit GFX Library/fontconvert/fontconvert.c) so
# the converter is reproducible from this repo without depending on the
# transient PlatformIO libdeps tree. Re-vendor on Adafruit_GFX upgrades
# if the upstream file changes.
#
# Build:
#     make           # auto-detects FreeType via pkg-config (works on
#                    # Linux, Apple Silicon Homebrew, Intel Homebrew).
#     make clean
#
# Requires: gcc (or clang), FreeType 2 + pkg-config.
# macOS:    brew install freetype pkg-config
# Linux:    apt install libfreetype-dev pkg-config
#
# Then convert with the wrapper:  ../convert_font.sh <ttf> <pt> <out.h>

CC      ?= cc
PKG_CFG ?= pkg-config

CFLAGS  := -Wall -O2 $(shell $(PKG_CFG) --cflags freetype2)
LIBS    := $(shell $(PKG_CFG) --libs freetype2)

all: fontconvert

fontconvert: fontconvert.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

clean:
	rm -f fontconvert

.PHONY: all clean
