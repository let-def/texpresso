all:
	$(MAKE) texpresso
	$(MAKE) texpresso-xetex
	@echo "# Build succeeded. Try running:"
	@echo "# build/texpresso test/simple.tex"

texpresso:
	$(MAKE) -C src/frontend texpresso

dev:
	$(MAKE) -C src texpresso-dev

debug:
	$(MAKE) -C src texpresso-debug texpresso-debug-proxy

clean:
	rm -rf build/*/*

distclean:
	rm -rf build Makefile.config

re2c:
	$(MAKE) -C src $@

UNAME := $(shell uname)

Makefile.config: Makefile
	$(MAKE) config

ifeq ($(UNAME), Linux)
config:
	mkdir -p build/objects
# LDCC: some Linux distribution build mupdf with C++ dependencies,
	echo >Makefile.config "CFLAGS=-O2 -ggdb -I. -fPIC"
	echo >>Makefile.config 'CC=gcc $$(CFLAGS)'
	echo >>Makefile.config 'LDCC=g++ $$(CFLAGS)'
	echo >>Makefile.config "LIBS=-lmupdf -lm `CC=gcc ./mupdf-config.sh` -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lgumbo -lSDL2"
endif

ifeq ($(UNAME), Darwin)
BREW=$(shell brew --prefix)
BREW_ICU4C=$(shell brew --prefix icu4c)
config:
	mkdir -p build/objects
	echo >Makefile.config "CFLAGS=-O2 -ggdb -I. -fPIC -I$(BREW)/include"
	echo >>Makefile.config 'CC=gcc $$(CFLAGS)'
	echo >>Makefile.config 'LDCC=g++ $$(CFLAGS)'
	echo >>Makefile.config "LIBS=-L$(BREW)/lib -lmupdf -lm `CC=gcc ./mupdf-config.sh -L$(BREW)/lib` -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lSDL2"
endif

texpresso-xetex:
	$(MAKE) -C src/engine

compile_commands.json:
	bear -- $(MAKE) -B -k all

.PHONY: all dev clean config texpresso-xetex re2c compile_commands.json
