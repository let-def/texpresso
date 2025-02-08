## Target name

XETEX_BIN?=xetex-bin

## Build configuration

PKG=freetype2 harfbuzz graphite2 fontconfig icu-uc libpng
CFLAGS = -Iinclude -Ilayout -Idpx $(shell $(PKG_CONFIG) $(PKG) --cflags) $(_CFLAGS) $(_FLAGS)
CXXFLAGS = -std=c++17 $(CFLAGS) $(_CXXFLAGS)
LDFLAGS = -lm -lz $(shell $(PKG_CONFIG) $(PKG) --libs) $(_LDFLAGS) $(_FLAGS)
DIRS=engine layout dpx main

## Build tools configuration

_FLAGS?=-fsanitize=address
_CC?=gcc
_CXX?=g++
_LD?=g++
_CFLAGS?=-O0 -ggdb
PKG_CONFIG=pkg-config

## Internal variables

BUILD_DIRS=build $(patsubst %,build/%, $(DIRS))

OBJECTS ?= $(foreach dir,$(DIRS), $(patsubst %.cpp,build/%.o, $(patsubst %.c,build/%.o, \
		$(wildcard $(dir)/*.c) \
		$(wildcard $(dir)/*.cpp) \
	)))

## Main targets

all: build/$(XETEX_BIN)

clean:
	rm -f  $(OBJECTS)

distclean:
	rm -rf build

compile_commands.json:
	bear -- $(MAKE) --always-make all _CC=clang _CXX=clang++ _LD=clang++

## Internal targets

build/$(XETEX_BIN): $(OBJECTS)
	$(_LD) $(LDFLAGS) -o $@ $^

build/%.o: %.c | $(BUILD_DIRS)
	$(_CC) $(CFLAGS) -c -o $@ $^

build/%.o: %.cpp | $(BUILD_DIRS)
	$(_CXX) $(CXXFLAGS) -c -o $@ $^

$(BUILD_DIRS):
	mkdir -p $@

undefined:
	$(MAKE) all 2>&1 | grep -o 'undefined reference to `.*'"'" | sort -u | cut -d ' ' -f 4

.PHONY: all clean distclean compile_commands.json
