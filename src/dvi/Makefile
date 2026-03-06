OBJECTS= \
	dvi_context.o dvi_interp.o dvi_prim.o dvi_special.o \
	dvi_scratch.o dvi_fonttable.o dvi_resmanager.o \
	tex_tfm.o tex_fontmap.o tex_vf.o tex_enc.o \
  vstack.o pdf_lexer.o

BUILD=../../build
DIR=$(BUILD)/frontend

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))

all: $(DIR)/libmydvi.a

$(DIR)/libmydvi.a: $(DIR_OBJECTS)
	ar cr $@ $^

$(DIR)/dvi_resmanager.o: dvi_resmanager.c
	$(CC) -I../include $(shell pkg-config --cflags freetype2) -c -o $@ $<

$(DIR)/%.o: %.c
	$(CC) -I../include -c -o $@ $<

$(DIR)/%.o: $(DIR)/%.c
	$(CC) -I../include -c -o $@ $<

%.c: %.re2c.c
	@ if test $@ -ot $^; then \
		echo -e >&2 "\033[0;33mre2c: $@ is older than $^ or missing"; \
	  echo -e >&2 "      consider running 'make re2c'\033[0m"; \
	fi

re2c:
	$(MAKE) -f Makefile.re2c all

clean:
	rm -f $(DIR)/libmydvi.a $(DIR_OBJECTS)

.PHONY: all clean re2c

../../Makefile.config:
	$(MAKE) -C ../.. config
include ../../Makefile.config
