OBJECTS=state.o fs.o incdvi.o myabort.o renderer.o engine_pdf.o engine_dvi.o synctex.o prot_parser.o sexp_parser.o json_parser.o editor.o

BUILD=../../build
DIR=$(BUILD)/frontend

# The TeX engine (engine_tex.c) runs in-process via a wasm2c-compiled engine, so
# building texpresso requires the wasm engine objects. Point WASM_ENGINE_DIR at
# the wasm2c build dir (from scripts/build-wasm2c-<engine>.sh); it must hold the
# wasm2c engine.o + wabt runtime .o.
#   make WASM_ENGINE_DIR=../../engines/build-wasm2c-xetex
ifndef WASM_ENGINE_DIR
$(error WASM_ENGINE_DIR is required (the wasm2c engine build dir))
endif
ifeq ($(wildcard $(WASM_ENGINE_DIR)/engine.c)$(wildcard $(WASM_ENGINE_DIR)/engine.o),)
$(error No engine in $(WASM_ENGINE_DIR). Get one with `make fetch-engine` (prebuilt, needs only a C compiler) or `make engine-source` (builds from TeX Live sources; needs emscripten + wabt))
endif
OBJECTS += engine_tex_xetex.o engine_tex_pdftex.o engine_tex_luatex.o
# A fetched engine bundle vendors the wabt runtime headers, so it needs no wabt
# install — which is what lets Linux/CI build without brew. Fall back to a
# local wabt only when building against a raw wasm2c output directory.
WABT_INC = $(if $(wildcard $(WASM_ENGINE_DIR)/wasm-rt.h),$(WASM_ENGINE_DIR),$(shell brew --prefix wabt)/include)

# One binary per engine. Most frontend objects are engine-independent and
# shared; the two that are not get per-engine object names so switching engines
# cannot reuse the wrong one:
#   wasm_host_txp — compiled against that engine's wasm2c engine.h
#   engine_tex    — has the engine baked in as TEXPRESSO_ENGINE, so the profile
#                   follows the engine that is actually linked instead of being
#                   guessed from the executable's path at runtime
TEX_ENGINE ?= xetex
TEXPRESSO_BIN ?= $(BUILD)/texpresso

WASM_LINK = $(WASM_ENGINE_DIR)/engine.o \
            $(WASM_ENGINE_DIR)/wasm-rt-impl.o \
            $(WASM_ENGINE_DIR)/wasm-rt-mem-impl.o \
            $(WASM_ENGINE_DIR)/wasm-rt-exceptions-impl.o \
            $(DIR)/wasm_host_txp-$(TEX_ENGINE).o \
            $(DIR)/engine_tex-$(TEX_ENGINE).o

$(DIR)/engine_tex-$(TEX_ENGINE).o: engine_tex.c | $(DIR)
	$(CC) -MMD -MP -DTEXPRESSO_ENGINE='"$(TEX_ENGINE)"' \
	  -c -o $@ -I../dvi/ -I../include $<


# An engine bundle ships portable C, not objects — compile it here. A local
# wasm2c build already leaves .o beside the .c, so this stays a no-op there.
$(WASM_ENGINE_DIR)/%.o: $(WASM_ENGINE_DIR)/%.c
	cc -O2 -I$(WABT_INC) -I$(WASM_ENGINE_DIR) -I../engine-wasm -c -o $@ $<

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))
TARGETS=texpresso texpresso-debug-proxy

all: $(TARGETS)

texpresso: $(TEXPRESSO_BIN)
$(TEXPRESSO_BIN): $(DIR)/driver.o $(DIR)/main.o $(DIR)/logo.o $(DIR_OBJECTS) $(WASM_LINK) $(DIR)/libmydvi.a $(BUILD)/common/libcommon.a
	$(LDCC) -o $@ $^ $(LIBS)

# host shim for texpresso: library mode (no main), against the chosen engine.
# Plain cc (not $(CC)) so the frontend's -I. doesn't shadow the wasm2c engine.h.
$(DIR)/wasm_host_txp-$(TEX_ENGINE).o: ../engine-wasm/wasm_host.c | $(DIR)
	cc -O2 -c -o $@ -DWASM_HOST_NO_MAIN -I$(WABT_INC) -I$(WASM_ENGINE_DIR) $<


texpresso-debug-proxy: $(BUILD)/texpresso-debug-proxy
$(BUILD)/texpresso-debug-proxy: proxy.c
	$(LDCC) -o $@ $^

$(BUILD)/common/libcommon.a:
	$(MAKE) -C ../common

re2c:
	$(MAKE) -C ../dvi $@

# -MMD -MP records each object's header dependencies, so editing a header
# rebuilds what included it. Without this a struct change left stale objects
# disagreeing about its size, and the mismatch corrupted the heap at runtime
# rather than failing to build.
$(DIR)/%.o: %.c | $(DIR)
	$(CC) -MMD -MP -c -o $@ -I../dvi/ -I../include $<

-include $(DIR_OBJECTS:.o=.d) $(DIR)/driver.d $(DIR)/main.d $(DIR)/logo.d

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
