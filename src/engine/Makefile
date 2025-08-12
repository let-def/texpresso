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
OBJSYS=$(BUILD)/layout/xetex-XeTeXFontMgr_Mac.o

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

BUILD=../../build
BUILD_DIRS=$(BUILD) $(patsubst %,$(BUILD)/%, $(DIRS))

OBJECTS ?= $(foreach dir,$(DIRS), $(patsubst %.cpp,$(BUILD)/%.o, $(patsubst %.c,$(BUILD)/%.o, \
		$(wildcard $(dir)/*.c) \
		$(wildcard $(dir)/*.cpp) \
	))) $(OBJSYS)

## Main targets

all: $(BUILD)/$(XETEX_BIN)

clean:
	rm -f  $(OBJECTS)

distclean:
	rm -rf $(BUILD)

## Internal targets

$(BUILD)/$(XETEX_BIN): $(OBJECTS) $(BUILD)/common/libcommon.a
	$(_LD) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: %.c | $(BUILD_DIRS)
	$(_CC) $(CFLAGS) -I../include -c -o $@ $^

$(BUILD)/%.o: %.cpp | $(BUILD_DIRS)
	$(_CXX) $(CXXFLAGS) -I../include -c -o $@ $^

$(BUILD)/%.o: %.mm | $(BUILD_DIRS)
	$(_CXX) $(CXXFLAGS) -I../include -c -o $@ $^

$(BUILD)/common/libcommon.a:
	$(MAKE) -C ../common

$(BUILD_DIRS):
	mkdir -p $@

undefined:
	$(MAKE) all 2>&1 | grep -o 'undefined reference to `.*'"'" | sort -u | cut -d ' ' -f 4

.PHONY: all clean distclean
