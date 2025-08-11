all:
	$(MAKE) texpresso
	$(MAKE) texpresso-tonic
	@echo "# Build succeeded. Try running:"
	@echo "# build/texpresso test/simple.tex"

texpresso:
	$(MAKE) -C src texpresso

dev:
	$(MAKE) -C src texpresso-dev

debug:
	$(MAKE) -C src texpresso-debug texpresso-debug-proxy

clean:
	rm -rf build/objects/*

distclean:
	rm -rf build Makefile.config
	cd tectonic && cargo clean

re2c:
	$(MAKE) -C src $@

test-utfmapping:
	gcc -g -o test/test_utf_mapping test/test_utf_mapping.c
	test/test_utf_mapping &> test/test_utf_mapping.output
	git diff --exit-code test/test_utf_mapping.output

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
	echo >>Makefile.config "TECTONIC_ENV="
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
	echo >>Makefile.config "TECTONIC_ENV=PKG_CONFIG_PATH=$(BREW_ICU4C)/lib/pkgconfig C_INCLUDE_PATH=$(BREW_ICU4C)/include LIBRARY_PATH=$(BREW_ICU4C)/lib"
endif

texpresso-tonic:
	$(MAKE) -f Makefile.tectonic tectonic
	cp -f tectonic/target/release/texpresso-tonic build/

.PHONY: all dev clean config texpresso-tonic re2c
