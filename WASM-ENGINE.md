# The TeX engine

TeXpresso runs a stock TeX engine inside its own process. Upstream TeX Live
sources are compiled to WebAssembly, then to portable C with wasm2c, and linked
into the texpresso binary. There is no separate engine process and no patched
engine source.

## Why wasm

Every syscall the engine makes is a wasm import the host implements, so file I/O
is routed to texpresso's virtual filesystem without touching the engine's
source. Adding an engine becomes a build task rather than a patching task, and
one host drives all of them.

The engine's linear memory is a single array the host owns, so a snapshot is a
memory copy under the host's control rather than an OS process operation.

## Build

Per engine, with the engine source unmodified:

    web2c sources -> emcc -> engine.wasm -> wasm2c -> engine.c -> cc -> texpresso

emcc runs non-standalone, which is what makes filesystem syscalls surface as
imports. wasm2c runs with `-n engine`, so every engine produces the same symbol
names and a single host file drives all of them.

The engine is not built by `make`. Either:

- `make fetch-engine` downloads a prebuilt `engine.c` bundle and compiles it.
  Needs only a C compiler.
- `make engine-source` builds from pinned TeX Live sources. Needs emscripten and
  wabt, and is much slower.

Both honour `TEXPRESSO_ENGINE`, which selects `xetex` (default), `pdftex` or
`luatex`, and also selects the engine profile compiled into the frontend.
Neither runs as part of `make`, so a build does not reach the network on its
own.

## Host

`src/engine-wasm/wasm_host.c` implements the wasm imports and the engine
lifecycle. File syscalls pass through a `wasm_io_ops` seam.
`src/frontend/engine_tex.c` backs that seam with texpresso's VFS, so the engine
reads and writes buffers texpresso owns and never touches the real filesystem.
The standalone build of the same host uses native I/O instead; `make
engine-native` produces it, and `test-fence` runs against it.

## Suspend and resume

The engine runs on a dedicated 32 MB stack via `ucontext`/`swapcontext` and
yields to the host from inside `fd_read`, its natural suspend point. TeX has a
deep C stack mid-run, so giving the engine its own stack is what lets a snapshot
capture the call stack as well as the heap.

A snapshot is the dirtied pages of linear memory, the wasm globals, the used top
of that stack, the register context, and file positions.

## Snapshots

Linear memory is one `mmap`'d region. Taking a layer marks it read-only; a
`SIGSEGV`/`SIGBUS` handler saves each page into that layer before the first
write to it; restoring copies the saved pages back. Layers stack, so restoring
to layer k applies the saved pages of every layer above it, then k's own.

Linux and macOS only. The host uses `ucontext` and POSIX signal handling; there
is no Windows implementation.

## Incremental replay

As the document is read, texpresso records checkpoints: a snapshot layer plus
the VFS mark, the document read position, the read-trace length and the fd
table. Checkpoints are spaced by elapsed time, so they cluster where the engine
spends compute.

On an edit, `compute_fences` walks the read trace backward from the edit with
exponentially growing time gaps, picks the checkpoint to restore, and chooses
where to lay new checkpoints during the replay.

Two cases do a full re-run instead: no checkpoint exists yet, or the selection
is checkpoint 0. Checkpoint 0 is taken at the job file's first read, which
happens before the format is undumped, because TeX reads the first line looking
for a `%&format` directive. Restoring it would replay the undump, so it costs a
full run and saves nothing.

`test-replay` asserts a replayed edit is byte-identical to a full compile, at
four positions. `test-fence` checks the snapshot layer with TeX and the frontend
out of the picture.

## Determinism

Time, randomness and file mtimes are virtualized through the import layer, since
replay is only correct if a re-run observes the same values.
`SOURCE_DATE_EPOCH` is honoured.

## Formats

No `.fmt` is shipped. If the format the engine needs is absent, it is generated
on first run and cached. Loading the LaTeX kernel is format generation, since
`latex.ltx` ends with `\dump`.

## Engine notes

xetex needs freetype2, harfbuzz, graphite2, teckit and ICU. Off macOS it wants
fontconfig, which has no wasm build, so a FreeType-backed shim in
`src/engine-wasm/fontconfig-shim/` implements the enumeration subset xetex uses.
ICU data is built as a loadable archive and mapped through the host, because
ICU's static data entry point is not consulted under wasm2c.

luatex uses PUC Lua, since LuaJIT cannot target wasm, and needs
`-sSTACK_SIZE=16MB`; the emscripten default overflows into the data segment.

pdftex needs `-sALLOW_MEMORY_GROWTH=1`.

`getdents64` is a stub, so the engine cannot enumerate directories. kpathsea
does not need it, but luaotfload does, so the first luatex run after generating
a format reports missing metric data for system fonts. Later runs are clean, and
TeX Live's own fonts resolve normally.

## See also

- `EDITOR-PROTOCOL.md`: the protocol between an editor and texpresso.
- `INSTALL.md`: build and install.
