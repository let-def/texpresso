## Target name

XETEX_BIN?=texpresso-xetex

UNAME := $(shell uname)

## System specific flags

ifeq ($(UNAME), Darwin)

BREW=$(shell brew --prefix)
BREW_ICU4C=$(shell brew --prefix icu4c)

CSYSFLAGS=-DXETEX_MAC -I $(BREW)/include -I $(BREW_ICU4C)/include -fno-common
LDSYSFLAGS=-L $(BREW_ICU4C)/lib -licuuc -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework AppKit
PKGSYS=
OBJSYS=../../build/layout/xetex-XeTeXFontMgr_Mac.o

else

CSYSFLAGS=
LDSYSFLAGS=
PKGSYS=icu-uc
OBJSYS=

endif

## Build tools configuration

_FLAGS?=
_CC?=gcc
_CXX?=g++
_LD?=g++
_CFLAGS?=-O2 
PKG_CONFIG=pkg-config

## Build configuration

PKG=freetype2 harfbuzz graphite2 fontconfig libpng $(PKGSYS)
CFLAGS = -Iinclude -Ilayout -Idpx $(shell $(PKG_CONFIG) $(PKG) --cflags) $(_CFLAGS) $(_FLAGS) $(CSYSFLAGS)
CXXFLAGS = -std=c++17 $(CFLAGS) $(_CXXFLAGS)
LDFLAGS = -lm -lz $(shell $(PKG_CONFIG) $(PKG) --libs) $(_LDFLAGS) $(_FLAGS) $(LDSYSFLAGS)
DIRS=engine layout dpx main


## Internal variables

BUILD_DIRS=../../build $(patsubst %,../../build/%, $(DIRS))

OBJECTS ?= $(foreach dir,$(DIRS), $(patsubst %.cpp,../../build/%.o, $(patsubst %.c,../../build/%.o, \
		$(wildcard $(dir)/*.c) \
		$(wildcard $(dir)/*.cpp) \
	))) $(OBJSYS)

## Main targets

all: ../../build/$(XETEX_BIN)

clean:
	rm -f  $(OBJECTS)

distclean:
	rm -rf ../../build

## Internal targets

../../build/$(XETEX_BIN): $(OBJECTS)
	$(_LD) $(LDFLAGS) -o $@ $^ ../../build/common/libcommon.a

../../build/%.o: %.c | $(BUILD_DIRS)
	$(_CC) $(CFLAGS) -I../include -c -o $@ $^

../../build/%.o: %.cpp | $(BUILD_DIRS)
	$(_CXX) $(CXXFLAGS) -I../include -c -o $@ $^

../../build/%.o: %.mm | $(BUILD_DIRS)
	$(_CXX) $(CXXFLAGS) -I../include -c -o $@ $^

$(BUILD_DIRS):
	mkdir -p $@

undefined:
	$(MAKE) all 2>&1 | grep -o 'undefined reference to `.*'"'" | sort -u | cut -d ' ' -f 4

.PHONY: all clean distclean
