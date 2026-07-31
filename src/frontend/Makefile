OBJECTS=sprotocol.o state.o fs.o incdvi.o myabort.o renderer.o engine_tex.o engine_pdf.o engine_dvi.o synctex.o prot_parser.o sexp_parser.o json_parser.o editor.o

BUILD=../../build
DIR=$(BUILD)/frontend

# Optional in-process wasm engine backend. Enable with e.g.:
#   make WASM_ENGINE_DIR=../../engines/build-wasm2c-xetex
# That dir must hold the wasm2c engine.o + wabt runtime .o (from
# scripts/build-wasm2c-<engine>.sh). WASM_DEF is threaded into every compile so
# main.c picks the wasm engine; WASM_LINK adds the engine to the texpresso link.
ifdef WASM_ENGINE_DIR
OBJECTS += engine_wasm.o engine_wasm_xetex.o engine_wasm_pdftex.o engine_wasm_luatex.o
WASM_DEF = -DHAVE_WASM_ENGINE
WABT_INC = $(shell brew --prefix wabt)/include
WASM_LINK = $(WASM_ENGINE_DIR)/engine.o \
            $(WASM_ENGINE_DIR)/wasm-rt-impl.o \
            $(WASM_ENGINE_DIR)/wasm-rt-mem-impl.o \
            $(WASM_ENGINE_DIR)/wasm-rt-exceptions-impl.o \
            $(DIR)/wasm_host_txp.o
endif

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))
TARGETS=texpresso texpresso-dev texpresso-debug-proxy texpresso.so

all: $(TARGETS)

texpresso: $(BUILD)/texpresso
$(BUILD)/texpresso: $(DIR)/driver.o $(DIR)/main.o $(DIR)/logo.o $(DIR_OBJECTS) $(WASM_LINK) $(DIR)/libmydvi.a $(BUILD)/common/libcommon.a
	$(LDCC) -o $@ $^ $(LIBS)

# host shim for texpresso: library mode (no main), against the chosen engine.
# Plain cc (not $(CC)) so the frontend's -I. doesn't shadow the wasm2c engine.h.
$(DIR)/wasm_host_txp.o: ../engine-wasm/wasm_host.c | $(DIR)
	cc -O2 -c -o $@ -DWASM_HOST_NO_MAIN -I$(WABT_INC) -I$(WASM_ENGINE_DIR) $<

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
	$(CC) -c -o $@ -I../dvi/ -I../include $(WASM_DEF) $<

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
