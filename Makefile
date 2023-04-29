all:
	$(MAKE) config
	$(MAKE) -C src texpresso

dev:
	$(MAKE) config
	$(MAKE) -C src texpresso-dev

clean:
	rm -rf build/objects/*

distclean:
	rm -rf build Makefile.config

UNAME := $(shell uname)

config: Makefile.config build/objects

build/objects:
	mkdir -p build/objects

ifeq ($(UNAME), Linux)
Makefile.config: Makefile
	echo >$@ "CC=gcc -O2 -ggdb -I. -fPIC"
	echo >>$@ "LIBS=-lmupdf -lm -lmupdf-third -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lgumbo -lSDL2"
endif

ifeq ($(UNAME), Darwin)
BREW=$(shell brew --prefix)
Makefile.config: Makefile
	echo >$@ "CC=gcc -O2 -ggdb -I. -fPIC -I$(BREW)/include"
	echo >>$@ "LIBS=-L$(BREW)/lib -lmupdf -lm -lmupdf-third -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lgumbo -lSDL2"
endif

.PHONY: all dev clean
