OBJECTS=sprotocol.o state.o fs.o incdvi.o myabort.o renderer.o engine_tex.o engine_pdf.o engine_dvi.o synctex.o sexp_parser.o

BUILD=../build
DIR=$(BUILD)/objects

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))
TARGETS=texpresso texpresso-dev texpresso-debug-proxy texpresso.so

all: $(TARGETS)

texpresso: $(BUILD)/texpresso
$(BUILD)/texpresso: $(DIR)/driver.o $(DIR)/main.o $(DIR)/logo.o $(DIR_OBJECTS) $(DIR)/libmydvi.a
	$(CC) -o $@ $^ $(LIBS)

texpresso-dev: $(BUILD)/texpresso-dev
$(BUILD)/texpresso-dev: $(DIR)/driver.o $(DIR)/loader.o $(DIR)/logo.o $(BUILD)/texpresso.so
	$(CC) -o $@ $^ $(LIBS)

texpresso.so: $(BUILD)/texpresso.so
$(BUILD)/texpresso.so: $(DIR)/main.o $(DIR_OBJECTS) $(DIR)/libmydvi.a
	$(MAKE) -C dvi
	$(CC) -ldl -shared -o $@ $^ $(LIBS)
	killall -SIGUSR1 texpresso-dev || true

texpresso-debug-proxy: $(BUILD)/texpresso-debug-proxy
$(BUILD)/texpresso-debug-proxy: proxy.c
	$(CC) -o $@ $^

texpresso-debug: $(BUILD)/texpresso-debug
$(BUILD)/texpresso-debug: ../scripts/texpresso-debug
	cp $< $@

$(DIR)/%.o: %.c
	$(CC) -c -o $@ -Idvi/ $<

$(DIR)/libmydvi.a: $(wildcard dvi/*.*)
	$(MAKE) -C dvi

clean:
	rm -f $(DIR)/*.o $(DIR)/*.a
	$(MAKE) -C dvi clean

../Makefile.config:
	$(MAKE) -C .. config
include ../Makefile.config

.PHONY: all clean $(TARGETS)
