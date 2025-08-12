OBJECTS=sprotocol.o state.o fs.o incdvi.o myabort.o renderer.o engine_tex.o engine_pdf.o engine_dvi.o synctex.o prot_parser.o sexp_parser.o json_parser.o editor.o

BUILD=../../build
DIR=$(BUILD)/frontend

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))
TARGETS=texpresso texpresso-dev texpresso-debug-proxy texpresso.so

all: $(TARGETS)

texpresso: $(BUILD)/texpresso
$(BUILD)/texpresso: $(DIR)/driver.o $(DIR)/main.o $(DIR)/logo.o $(DIR_OBJECTS) $(DIR)/libmydvi.a $(BUILD)/common/libcommon.a
	$(LDCC) -o $@ $^ $(LIBS)

texpresso-dev: $(BUILD)/texpresso-dev
$(BUILD)/texpresso-dev: $(DIR)/driver.o $(DIR)/loader.o $(DIR)/logo.o | $(BUILD)/texpresso-dev.so
	$(LDCC) -o $@ $^ $(LIBS)

texpresso-dev.so: $(BUILD)/texpresso-dev.so
$(BUILD)/texpresso-dev.so: $(DIR)/main.o $(DIR_OBJECTS) $(DIR)/libmydvi.a
	$(MAKE) -C ../dvi
	$(LDCC) -ldl -shared -o $@ $^ $(LIBS)
	killall -SIGUSR1 texpresso-dev || true

texpresso-debug-proxy: $(BUILD)/texpresso-debug-proxy
$(BUILD)/texpresso-debug-proxy: proxy.c
	$(LDCC) -o $@ $^

texpresso-debug: $(BUILD)/texpresso-debug
$(BUILD)/texpresso-debug: ../../scripts/texpresso-debug
	cp $< $@

$(BUILD)/common/libcommon.a:
	$(MAKE) -C ../common

re2c:
	$(MAKE) -C ../dvi $@

$(DIR)/%.o: %.c | $(DIR)
	$(CC) -c -o $@ -I../dvi/ -I../include $<

$(DIR)/libmydvi.a:
	$(MAKE) -C ../dvi $(DIR)/libmydvi.a

clean:
	rm -f $(DIR)/*.o $(DIR)/*.a
	$(MAKE) -C ../dvi clean

$(DIR):
	mkdir -p $@

../../Makefile.config:
	$(MAKE) -C ../.. config

include ../../Makefile.config

.PHONY: all clean $(TARGETS) re2c $(DIR)/libmydvi.a
