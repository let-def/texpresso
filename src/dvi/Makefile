OBJECTS= \
	dvi_context.o dvi_interp.o dvi_prim.o dvi_special.re2c.o \
	dvi_scratch.o dvi_fonttable.o dvi_resmanager.o \
	tex_tfm.o tex_fontmap.o tex_vf.o tex_enc.o \
    vstack.o pdf_lexer.re2c.o

BUILD=../../build
DIR=$(BUILD)/objects

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))

all: $(DIR)/libmydvi.a

$(DIR)/libmydvi.a: $(DIR_OBJECTS)
	ar cr $@ $^

$(DIR)/dvi_resmanager.o: dvi_resmanager.c
	$(CC) $(shell pkg-config --cflags freetype2) -c -o $@ $<

$(DIR)/%.re2c.c: %.c
	re2c $< -o $@ --tags --bit-vectors

$(DIR)/%.o: %.c
	$(CC) -c -o $@ $<

$(DIR)/%.o: $(DIR)/%.c
	$(CC) -c -o $@ $<

clean:
	rm -f $(DIR)/libmydvi.a $(DIR_OBJECTS) $(DIR)/dvi_special.c.re2c.c

.PHONY: all clean

../../Makefile.config:
	$(MAKE) -C ../.. config
include ../../Makefile.config
