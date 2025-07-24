all:
	$(MAKE) common texpresso texpresso-xetex
	@echo "# Build succeeded."
	@echo "# First time launch needs to download many files and can be slow."
	@echo "# You can speed-up this process using:"
	@echo "#"
	@echo "#   make fill-tectonic-cache"
	@echo "#"
	@echo "# After, you can try texpresso by running:"
	@echo "#"
	@echo "#   build/texpresso test/simple.tex"
	@echo "#"

common:
	$(MAKE) -C src/common

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
	echo >>Makefile.config "LIBS=-lmupdf -lm `CC=gcc ./mupdf-config.sh` -lz -ljpeg -lharfbuzz -lfreetype -lgumbo -lSDL2"
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

fill-tectonic-cache:
	tectonic --outfmt fmt test/format.tex
	tectonic --outfmt xdv test/simple.tex

.PHONY: all dev clean config texpresso common texpresso-xetex re2c compile_commands.json fill-tectonic-cache
