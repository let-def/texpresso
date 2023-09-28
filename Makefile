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

UNAME := $(shell uname)

config: Makefile.config build/objects

build/objects:
	mkdir -p build/objects

ifeq ($(UNAME), Linux)
Makefile.config: Makefile
	echo >$@ "CC=gcc -O2 -ggdb -I. -fPIC"
	echo >>$@ "LIBS=-lmupdf -lm $(shell ./mupdf-config.sh) -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lgumbo -lSDL2"
	echo >>$@ "TECTONIC_ENV="
endif

ifeq ($(UNAME), Darwin)
BREW=$(shell brew --prefix)
BREW_ICU4C=$(shell brew --prefix icu4c)
Makefile.config: Makefile
	echo >$@ "CC=gcc -O2 -ggdb -I. -fPIC -I$(BREW)/include"
	echo >>$@ "LIBS=-L$(BREW)/lib -lmupdf -lm -lmupdf-third -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lgumbo -lSDL2"
	echo >>$@ "TECTONIC_ENV=PKG_CONFIG_PATH=$(BREW_ICU4C)/lib/pkgconfig"

endif

texpresso-tonic:
	$(MAKE) -f Makefile.tectonic tectonic
	cp -f tectonic/target/release/texpresso-tonic build/

.PHONY: all dev clean texpresso-tonic
