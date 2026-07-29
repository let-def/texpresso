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

**Status: proven.** `wasm_host.c` snapshots at the first read, runs a full
typeset (run A), rolls back (restore memory + stack + context + fd positions),
and re-runs from the snapshot (run B). Both runs emit byte-identical output
(`TEXPRESSO_SNAPSHOT_TEST=1` → `SNAPSHOT ROLLBACK PASS`).

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
| 2 | Coroutine-stack + COW snapshot on linear memory; validate rollback == re-run | snapshot correctness — **done (full-copy; PASS)** |
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
- Full-copy snapshot cost vs mprotect dirty-tracking on real documents — the
  linear memory is mmap fixed-base, so mprotect COW is a drop-in optimization.
- COW granularity vs snapshot frequency — page-level dirty tracking cost.
- Host I/O state at rollback: file positions/open-fds must be snapshotted too
  (the proof defers closes + resets positions; texpresso's VFS owns this).
- luatex build complexity under emscripten (heaviest engine).
- Do we keep the fork engine long-term (Unix speed) or unify on wasm-COW?
