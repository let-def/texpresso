# WASM Engine — Branch Objective & Specification

Branch: `eclipse`  ·  Status: fresh start (based on `main` @ post-#149)
Old multi-engine scaffolding archived at `archive/texpresso-eclipse`.

## 1. Objective

Replace texpresso's `fork()`-based engine snapshotting with an in-process,
sandboxed engine model, and unify all TeX engines behind a single host
interface. Concretely:

1. Compile each TeX engine (**pdftex → xetex → luatex**, in that order) to
   WebAssembly, then to native C via `wasm2c`.
2. Drive every engine through **one** host-side interface — texpresso sees "a
   wasm module + an import table," not a forked process.
3. Snapshot/rollback via **userland copy-on-write** on the wasm linear memory,
   not `fork()`.
4. Ship on **Linux, macOS, and Windows** from one codebase.

## 2. Why (motivation)

- **Windows support.** The current engine relies on `fork()` + OS copy-on-write,
  which Windows lacks. This is the fundamental blocker. Moving snapshots into
  userland on the wasm heap, and the engine in-process, removes `fork()` and the
  whole subprocess/IPC layer with it.
- **No per-engine source patching.** Today each engine is a patched build
  (`texpresso-xetex`) that routes I/O through the protocol. With wasm, every
  syscall is an import the host implements — I/O interception is universal and
  requires **zero engine source changes**. Adding an engine becomes a build task,
  not a patching task.
- **Determinism & sandboxing.** All engine memory is one linear array we own;
  all syscalls cross a boundary we control. Snapshots and replay become exact.

## 3. Architecture

### 3.1 Build pipeline (per engine, unchanged engine source)
```
engine.c (stock web2c source, unmodified)
  │  emcc              Emscripten: clang wasm backend + libc (non-standalone)
  ▼
engine.wasm            (no asyncify — suspend/resume is a host coroutine, 3.3)
  │  wasm2c            generate portable C emulating the wasm
  ▼
engine_wasm.c (machine-generated; never hand-edited)
  │  clang/gcc/MSVC    compile + link into texpresso
  ▼
native object
```
The engine's C is never rewritten — wasm is a compile *target*. Only the host
side (imports, COW, driver) is written by hand, and it is engine-agnostic.

### 3.2 Unified host interface
A single `txp_engine` vtable implementation backed by a wasm module:
- **init** — instantiate the wasm module, allocate its linear memory.
- **import table** — the syscalls the engine calls (`open`/`read`/`write`/
  `close`/time/random) routed to texpresso's existing VFS (`state.c`).
- **step** — drive the engine to its next yield point.
- **snapshot / restore** — COW on the linear memory (see 3.4).
One implementation drives pdftex, xetex, luatex identically.

### 3.3 Snapshotting via a coroutine stack (the make-or-break piece — proven)
`fork()` captures the whole process including the **native call stack**.
Userland COW captures only memory. TeX has a deep C stack mid-run, so heap-only
COW cannot snapshot a live execution — the call stack must be capturable too.

Rather than Asyncify (which rewrites the module to spill its stack into linear
memory, at a runtime cost), we run the engine on a **dedicated fixed-address
stack** via `ucontext`/`swapcontext` (Windows fibers). The engine yields back to
the host from inside `fd_read` (its natural suspend point). A snapshot is then
just: copy the linear memory + copy that stack + save the register context. This
is fork-equivalent, needs **zero wasm instrumentation**, and adds no per-call
overhead. Asyncify is not used.

The linear memory itself is snapshotted by **mprotect dirty-tracking** (§3.4):
snapshot marks it read-only (O(1)); a SIGSEGV/SIGBUS handler saves each page on
first write; rollback copies back only the dirtied pages. The engine stack +
register context + fd positions are full-copy (small).

**Status: proven.** `wasm_host.c` snapshots at the first read, runs a full
typeset (run A), rolls back, and re-runs from the snapshot (run B). Both runs
emit byte-identical output, and only ~1.7% of heap pages (18/1084) are dirtied
by a full typeset (`TEXPRESSO_SNAPSHOT_TEST=1` → `SNAPSHOT ROLLBACK PASS`).

### 3.4 Copy-on-write snapshot layer
The wasm linear memory is one `mmap`'d region. Snapshot = mark read-only; a
fault handler saves each page on first write (dirty tracking); rollback restores
saved pages. Per-OS shim (one small file each):
- **Linux:** `userfaultfd` or `mprotect` + `SIGSEGV`.
- **macOS:** `mprotect` + `SIGBUS` / Mach exception ports.
- **Windows:** `VirtualProtect` + `PAGE_GUARD` + vectored exception handler.
Also snapshot wasm globals (small) and host-side VFS/file-position state
(texpresso already models the VFS).

### 3.5 Determinism
Virtualize all nondeterministic inputs through the import layer: `\time`/`\year`,
`os.time`, `\pdfuniformdeviate`/random, file mtimes. Required for
snapshot+replay correctness.

## 4. Scope

### In scope
- pdftex, xetex, luatex as wasm2c engines behind the unified interface.
- Userland COW snapshot backend (Linux + macOS first, then Windows).
- Import/VFS layer bridging engine syscalls to texpresso state.
- Coexistence with the existing fork engine during migration.

### Non-goals (for this branch)
- Rewriting engine internals or the typesetting logic.
- Browser/JS execution (we target native via wasm2c, not a JS runtime).
- Replacing the frontend/renderer/protocol (unchanged).
- LuaJIT (luatex must build with PUC Lua; LuaJIT can't target wasm).

## 5. Key decisions

- **wasm2c (AOT → C), not an embedded wasm runtime.** Self-contained portable C,
  no heavyweight runtime dependency; fits texpresso's minimalism. Cost: we build
  the COW + Asyncify plumbing ourselves.
- **Build stock web2c from source under emscripten.** No SwiftLaTeX, no
  prebuilt engines: raw upstream TeX Live sources, our own build scripts
  (`scripts/build-wasm-*.sh`), our import layer. Non-standalone so filesystem
  syscalls surface as imports the host implements (the VFS hook).
- **Suspend/resume is a host coroutine, not Asyncify.** See 3.3.
- **Engine source stays stock.** All interception at the wasm import boundary.

## 6. Toolchain
- `emcc` (Emscripten) — C → wasm (non-standalone; `-sSUPPORT_LONGJMP=wasm
  -fwasm-exceptions`, no JS runtime used).
- `wasm2c` (wabt) — wasm → portable C (`--enable-exceptions`).
- native compiler (clang/gcc/MSVC) — link the module + `wasm_host.c`.
- No asyncify/binaryen: suspend/resume is a host-side `ucontext` coroutine.

## 7. Phased plan

| Phase | Deliverable | Gate |
|-------|-------------|------|
| 0 | **pdftex feasibility spike** — pdftex → wasm → wasm2c, render `simple.tex`, measure wall-time vs native fork engine | perf acceptable? |
| 1 | Import/VFS layer + unified `txp_engine` wasm backend driving pdftex with **zero engine patches** | one full compile, no source edits |
| 2 | Coroutine-stack + mprotect-COW snapshot on linear memory; validate rollback == re-run | snapshot correctness — **done (mprotect COW; PASS, 18/1084 pages)** |
| 3 | Wire behind `txp_engine` vtable alongside fork engine; xetex next | **xetex renders via wasm2c (glyphs in XDV)** |
| 3.1 | In-process incremental replay via COW snapshot (per session) | **done — edits re-read + re-render correctly (self-test modes 1/2 PASS); format kept loaded, no re-instantiate.** Follow-up: `.aux` read-back at `\enddocument` aborts on replay (kpathsea `.`-db not resurfaced); pages correct, cross-ref convergence pending |
| 4 | luatex (PUC Lua) | **luatex runs via wasm2c + executes Lua 5.3** (bare `--ini` shipout: WIP) |

### luatex (Phase 4 notes)
luatex compiles to wasm (4.35 MB) with PUC Lua (lua53), no ICU/fontconfig
(fonts are handled in Lua). Libs: zlib libpng freetype2 graphite2 harfbuzz pplib
zziplib lua53. Same harfbuzz/freetype fixes as xetex; kpathsea is a *top-level*
build subdir (built by the top `make`, not `make -C texk`). Runs via the SAME
`wasm_host.c` (28 extra imports added: luasocket networking — stubbed;
`chdir/dup/dup3/fstat64/linkat/mkdirat/symlinkat/utimensat/fd_sync` — real;
`mktime_js`, `emscripten_get_heap_max`, sighandler/keepalive/DNS — impl/stub).
Proven: `\directlua` executes Lua 5.3, `kpse.var_value` resolves paths,
`--version` works. **Follow-up:** bare `--ini \shipout` aborts in kpathsea
(`assert(kpse->program_name)`) — the output-file open returns NULL (no format/
backend loaded in bare --ini) and the TEXMFOUTPUT fallback asserts. Needs a
format/backend or the texpresso I/O driver; not an engine/wasm2c blocker.

### xetex font stack (Phase 3 notes)
xetex compiles to wasm with freetype2 + harfbuzz + graphite2 + teckit + icu.
It requires fontconfig off-Mac, which has no wasm build; we link a minimal
**FreeType-backed fontconfig shim** (`src/engine-wasm/fontconfig-shim/`) that
implements only the enumeration subset xetex uses (`FcFontList` + metadata
accessors — no matching, cache, or `fonts.conf`), driven by a manifest of font
paths (`$TEXPRESSO_FONT_MANIFEST`). Cross-build quirks handled in
`scripts/build-wasm-xetex.sh`: harfbuzz `-Werror` pragmas, ICU native-tool build
under `emmake`, ICU emscripten platform config, freetype dropping wasm flags.
New host imports xetex needs beyond pdftex: `_mmap_js`, `_munmap_js`
(`_mmap_js` is implemented file-backed via the engine's `memalign` + `pread`;
`fcntl64`/`ioctl` were already present for pdftex).

**wasm2c + unified host (done):** both engines are generated with `wasm2c -n
engine`, so one `wasm_host.c` drives pdftex and xetex (import symbols are keyed
on `env`/`wasi`, not the engine). xetex-native runs a full pass and writes XDV
through the host I/O. xetex.wasm is linked `-sALLOW_MEMORY_GROWTH=1` (it xmallocs
~72 MB at startup vs the 16 MB default heap).

**ICU data (solved):** ICU's static data entry point isn't consulted at runtime
under wasm2c (`ucnv_open` → `U_FILE_ACCESS_ERROR`). Fix: build ICU data as a
loadable archive (`--with-data-packaging=archive` → `icudt78l.dat`), pass the
host environment through (`environ_get`) so the engine sees `ICU_DATA`, and load
the `.dat` via the host's file `_mmap_js`. xetex.wasm drops from 24 MB to 3.3 MB
(no embedded data). A bracket-path font (`\font\x="[/abs/font.ttf]"`) now renders
glyphs into the XDV. Run with `ICU_DATA=<dir with icudt78l.dat>`.
| 5 | Windows COW shim + port frontend I/O; Windows build | runs on Windows |

## 8. Success criteria
- All three engines render via the wasm backend with no engine source patches.
- Incremental edit → snapshot/rollback correctness matches the fork engine.
- Runs on Linux, macOS, and Windows.
- Per-keystroke latency within an acceptable factor of the fork engine
  (target set after the Phase 0 measurement).

## 9. Open questions
- **Perf (measured):** wasm2c pdftex vs native pdftex (TL2025), 2,000,000-iter
  compute-bound doc (box build + arithmetic + macro expansion): ~4.49 s vs
  ~3.37 s → **~1.33× overhead** (~1.35–1.4× pure compute). wasm2c startup is
  *lower* (47 ms vs 154 ms — skips kpathsea/texmf init). Conclusion: **wasm-only
  is viable** — 1.3× compute, and COW snapshots mean only the post-edit delta
  re-runs per keystroke. Hybrid (fork on Unix) not required for perf. Re-measure
  on real docs after VFS wiring.
- mprotect COW is in (18/1084 pages dirtied for a full typeset). Remaining: the
  engine stack is still full-copy — track its used extent (via saved SP) if the
  32 MiB copy shows up in per-keystroke latency.
- COW granularity vs snapshot frequency on larger real documents.
- Host I/O state at rollback: file positions/open-fds must be snapshotted too
  (the proof defers closes + resets positions; texpresso's VFS owns this).
- luatex build complexity under emscripten (heaviest engine).
- Do we keep the fork engine long-term (Unix speed) or unify on wasm-COW?
  (Perf answer above suggests wasm-only is acceptable; decide after VFS wiring +
  a real-doc re-measure.)
