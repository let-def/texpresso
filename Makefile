all:
	$(MAKE) Makefile.config
	$(MAKE) -C src

clean:
	$(MAKE) -C src clean

UNAME := $(shell uname)

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

.PHONY: all clean
