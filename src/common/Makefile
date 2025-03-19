OBJECTS= utils.o tectonic_provider.o texlive_provider.o

BUILD=../../build
DIR=$(BUILD)/common

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))

all: $(DIR)/libcommon.a

$(DIR)/libcommon.a: $(DIR_OBJECTS)
	ar cr $@ $^

$(DIR)/%.o: %.c | $(DIR)
	$(CC) -I../include -c -o $@ $<

$(DIR):
	mkdir -p $(DIR)

clean:
	rm -f $(DIR)/libcommon.a $(DIR_OBJECTS)

.PHONY: all clean re2c

../../Makefile.config:
	$(MAKE) -C ../.. config
include ../../Makefile.config
