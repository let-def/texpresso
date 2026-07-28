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
  │  emcc              Emscripten: clang wasm backend + libc, + Asyncify pass
  ▼
engine.wasm
  │  wasm-opt --asyncify   spill call stack into linear memory (see 3.3)
  ▼
engine.async.wasm
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

### 3.3 Snapshotting requires Asyncify (the make-or-break piece)
`fork()` captures the whole process including the **native call stack**.
Userland COW captures only memory. TeX has a deep C stack mid-run, so heap-only
COW cannot snapshot a live execution — **unless the call stack lives in the
linear memory**. Asyncify (a wasm transform) rewrites the module to spill its
call stack into linear memory and support suspend/resume. With Asyncify,
snapshotting the linear memory = capturing the full execution state =
fork-equivalent. Cost: Asyncify adds runtime overhead; measure it.

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
- **Build from source using SwiftLaTeX's recipes as a base.** Don't reinvent the
  web2c → emscripten build; fork proven recipes and add our Asyncify flags +
  import layer. Prebuilt engines can't be reused directly because they lack
  Asyncify.
- **Engine source stays stock.** All interception at the wasm import boundary.

## 6. Toolchain
- `emcc` (Emscripten) — C → wasm.
- `wasm-opt --asyncify` (binaryen) — stack-in-memory transform.
- `wasm2c` (wabt) — wasm → portable C.
- native compiler (clang/gcc/MSVC) — link into texpresso.

## 7. Phased plan

| Phase | Deliverable | Gate |
|-------|-------------|------|
| 0 | **pdftex feasibility spike** — pdftex → wasm → wasm2c, render `simple.tex`, measure wall-time vs native fork engine | perf acceptable? |
| 1 | Import/VFS layer + unified `txp_engine` wasm backend driving pdftex with **zero engine patches** | one full compile, no source edits |
| 2 | Asyncify + COW snapshot on linear memory; validate rollback == re-run on a scripted edit sequence | snapshot correctness |
| 3 | Wire behind `txp_engine` vtable alongside fork engine; xetex next | xetex renders |
| 4 | luatex (PUC Lua) | luatex renders |
| 5 | Windows COW shim + port frontend I/O; Windows build | runs on Windows |

## 8. Success criteria
- All three engines render via the wasm backend with no engine source patches.
- Incremental edit → snapshot/rollback correctness matches the fork engine.
- Runs on Linux, macOS, and Windows.
- Per-keystroke latency within an acceptable factor of the fork engine
  (target set after the Phase 0 measurement).

## 9. Open questions
- Asyncify overhead on real documents — acceptable for interactive latency?
- COW granularity vs snapshot frequency — page-level dirty tracking cost.
- luatex build complexity under emscripten (heaviest engine).
- Do we keep the fork engine long-term (Unix speed) or unify on wasm-COW?
