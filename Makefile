# The TeX engine runs in-process from a wasm2c build; point this at it (built by
# scripts/build-wasm-<eng>.sh + scripts/build-wasm2c-<eng>.sh).
WASM_ENGINE_DIR ?= engines/build-wasm2c-xetex

all: | Makefile.config
	$(MAKE) common texpresso
	@echo "# Build succeeded."
	@echo "# TeXpresso detects package providers (TeXlive or Tectonic) by looking in PATH:"
	@echo "# - it defaults to Tectonic if the 'tectonic' command is available"
	@echo "# - it falls back to TeXlive if the 'kpsewhich' command is available"
	@echo "# A provider can be selected manually by passing the '-texlive' or '-tectonic' flags."
	@echo "#"
	@echo "# When using tectonic, first time launch needs to download many files and can be slow."
	@echo "# You can speed-up this process using:"
	@echo "#   make fill-tectonic-cache"
	@echo "#"
	@echo "# After, you can try texpresso by running:"
	@echo "#   build/texpresso test/simple.tex"
	@echo "#"
	@echo "# Or:"
	@echo "#   build/texpresso -texlive test/simple.tex"
	@echo "#   build/texpresso -tectonic test/simple.tex"

common: | Makefile.config
	$(MAKE) -C src/common

texpresso: | Makefile.config
	$(MAKE) -C src/frontend texpresso WASM_ENGINE_DIR=$(abspath $(WASM_ENGINE_DIR))

# ---- Getting an engine ------------------------------------------------------
# Tier 1 (engine developers): build from pinned upstream TeX Live sources.
#   make engine-source ENGINE=xetex     # needs emscripten + wabt; slow
# Tier 2 (everyone else): download the prebuilt engine.c bundle and compile it.
#   make fetch-engine ENGINE=xetex      # needs only a C compiler
# Neither runs as part of `make all`: a build should not reach the network on
# its own.
ENGINE ?= xetex

fetch-engine:
	bash scripts/fetch-wasm-engine.sh $(ENGINE)

engine-source:
	bash scripts/fetch-engines.sh
	bash scripts/build-wasm-$(ENGINE).sh
	bash scripts/build-wasm2c-$(ENGINE).sh

# Model A: one binary per engine (build/texpresso-<eng>), each linking only its
# own wasm2c engine. The engine profile is picked at runtime from the binary
# name, so the suffix is load-bearing. Each needs engines/build-wasm2c-<eng>
# (scripts/build-wasm2c-<eng>.sh) and its format (scripts/build-wasm-fmt.sh <eng>).
TEX_ENGINES = xetex pdftex luatex
ENGINE_BINS = $(addprefix texpresso-,$(TEX_ENGINES))

$(ENGINE_BINS): texpresso-%: | Makefile.config
	$(MAKE) common
	$(MAKE) -C src/frontend texpresso \
	  TEX_ENGINE=$* \
	  WASM_ENGINE_DIR=$(abspath engines/build-wasm2c-$*) \
	  TEXPRESSO_BIN=../../build/texpresso-$*

engines: $(ENGINE_BINS)
	@echo "# Built: $(addprefix build/texpresso-,$(TEX_ENGINES))"

# Hot-reload dev builds are gone: the engine runs in this process, so reloading
# the frontend cannot reload it. What remains is the fifo proxy the Emacs
# integration attaches a debugger through.
debug-proxy: | Makefile.config
	$(MAKE) -C src/frontend texpresso-debug-proxy WASM_ENGINE_DIR=$(abspath $(WASM_ENGINE_DIR))

clean:
	rm -rf build/*/*

distclean:
	rm -rf build Makefile.config

re2c:
	$(MAKE) -C src/dvi $@

test-utfmapping:
	mkdir -p build
	gcc -g -o build/test_utf_mapping test/test_utf_mapping.c
	build/test_utf_mapping &> test/test_utf_mapping.output
	git diff --exit-code test/test_utf_mapping.output

UNAME := $(shell uname)

Makefile.config: Makefile
	$(MAKE) config

ifeq ($(UNAME), Linux)
config:
	mkdir -p build/objects
# LDCC: some Linux distribution build mupdf with C++ dependencies,
	echo >Makefile.config "CFLAGS=-O2 -ggdb -I. -fPIC"
	echo >>Makefile.config 'CC=gcc $$(CFLAGS)'
	echo >>Makefile.config 'LDCC=g++ $$(CFLAGS)'
	echo >>Makefile.config "LIBS=-lmupdf -lm `CC=gcc ./mupdf-config.sh` -lz -ljpeg -lharfbuzz -lfreetype -lSDL2"
endif

ifeq ($(UNAME), Darwin)
BREW=$(shell brew --prefix)
BREW_ICU4C=$(shell brew --prefix icu4c)
config:
	mkdir -p build/objects
	echo >Makefile.config "CFLAGS=-O2 -ggdb -I. -fPIC -I$(BREW)/include"
	echo >>Makefile.config 'CC=gcc $$(CFLAGS)'
	echo >>Makefile.config 'LDCC=g++ $$(CFLAGS)'
	echo >>Makefile.config "LIBS=-L$(BREW)/lib -lmupdf -lm `CC=gcc ./mupdf-config.sh -L$(BREW)/lib` -lz -ljpeg -lharfbuzz -lfreetype -lSDL2"
endif

compile_commands.json:
	bear -- $(MAKE) -B -k all

fill-tectonic-cache:
	tectonic --outfmt fmt test/format.tex
	tectonic --outfmt xdv test/simple.tex

test-open-base64:
	printf '(open-base64 "test/simple.tex" "%s")\n' "$$(base64 < test/simple.tex | tr -d '\n')" | \
	env SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/simple.tex

test-texpresso:
	env SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/simple.tex

test-texpresso-texlive:
	env SDL_VIDEODRIVER=dummy build/texpresso -texlive -test-initialize test/simple.tex

test-texpresso-tectonic:
	env SDL_VIDEODRIVER=dummy build/texpresso -tectonic -test-initialize test/simple.tex

test-stream:
	bash test/test_stream.sh

test-register:
	bash test/test-register.sh

test-lookup-file:
	bash test/test-lookup-file.sh

test-rerun:
	bash test/test-rerun.sh

macos-app: texpresso
	@[ "$$(uname)" = "Darwin" ] || { echo "macos-app requires macOS"; exit 1; }
	bash scripts/build-macos-app.sh

.PHONY: all debug-proxy clean config texpresso common engines fetch-engine engine-source $(ENGINE_BINS) re2c compile_commands.json fill-tectonic-cache test-texlive test-tectonic test-texpresso test-stream test-open-base64 test-register test-lookup-file test-rerun macos-app
