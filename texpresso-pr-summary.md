# TeXpresso VSCode Built-in Preview — PR Summary

## Motivation

TeXpresso originally only supported previewing LaTeX compilation results through an external SDL2 window. The goal is to enable built-in preview inside VSCode, achieving near-real-time incremental rendering comparable to the external SDL window experience.

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│  VSCode Webview Panel (Canvas Viewer)                    │
│  - base64 QOI data → JS decode → ImageBitmap → Canvas    │
│  - Zoom(Ctrl+wheel)/Pan/Nav/Dark Mode/i18n               │
│  - SyncTeX bidirectional: click→source / cursor→scroll    │
│  - Nav Bar: Home|◀|[Page]/Total|▶|End|↺|Res|[Options▾]  │
└──────────┬───────────────────────────────────────────────┘
           │ postMessage (base64 image data + control messages)
┌──────────▼───────────────────────────────────────────────┐
│  Extension Host (TypeScript)                             │
│  - preview.ts: WebviewPanel lifecycle                     │
│  - extension.ts: command registration, message routing    │
│  - texpresso stdout JSON → intercept page messages → fwd  │
└──────────┬───────────────────────────────────────────────┘
           │ ① stdin/stdout JSON protocol (commands + sync)
           │ ② Temp files (QOI image data, /dev/shm)
┌──────────▼───────────────────────────────────────────────┐
│  texpresso C Backend (-webview mode)                     │
│  - Skip SDL window, retain SDL event system               │
│  - MuPDF render → fz_pixmap → QOI → temp file → stdout    │
│  - Incremental: old vs new pixmap → dirty rects → diff    │
│  - New commands: synctex-backward, set-page, go-home, etc │
└───────────────────────────────────────────────────────────┘
```

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Image encoding | **QOI** | Already in codebase; 20-29× faster than PNG (~2.9ms vs ~83.8ms) |
| Transport | **Temp file + base64 postMessage** | VSCode postMessage only supports JSON; base64 is most efficient for binary |
| Temp file location | `/dev/shm/` | tmpfs avoids physical disk I/O |
| Webview frontend | **Custom Canvas viewer** | Fully controllable rendering pipeline |
| Mode coexistence | `-webview` CLI flag | Without flag: original SDL behavior unchanged |
| C-side auto-detect | `-resolution N` flag | Linked to VSCode setting `defaultResolution` |

## Feature List

### texpresso C Backend

- **Webview output** (`webview_output.c`): MuPDF render → QOI encode → write to `/dev/shm` → stdout JSON with file path
- **Incremental rendering (dirty rects)**: Per-pixel comparison of old vs new pixmap → when changed area < 50%, transmit only changed rects as QOI → `page-diff` message
- **New commands**: `synctex-backward`, `set-page`, `set-output-size`, `go-home`, `go-end`, `reset-zoom`, `set-fit-mode`, `invert`
- **`-webview` mode**: Skip SDL window creation, only init `SDL_INIT_TIMER | SDL_INIT_EVENTS`
- **`-resolution N`**: Control first-render resolution multiplier, linked to VSCode settings
- **Polling engine advancement**: `go-end`/`set-page` continuously poll the engine subprocess until terminated or target page found (`step()` returns false during computation — keep polling)
- **Forward SyncTeX**: Output `synctex-scroll` message (TeX coordinates) to drive preview scrolling
- **Backward SyncTeX**: Receive preview click coordinates → `synctex_scan()` → output source file location

### texpresso-vscode Extension

- **Preview panel** (`preview.ts`): WebviewPanel lifecycle, QOI file read + base64 encode, message forwarding
- **Canvas viewer** (`webview/index.ts`):
  - Pure-JS QOI decoder (~80 lines)
  - LRU page cache (configurable 1-50 pages)
  - Ctrl+wheel zoom (2.5% steps), Ctrl+click pan
  - Inertia scrolling (H+V, configurable speed)
  - Dark mode (CSS filter, persisted to localStorage)
  - Forward SyncTeX: cursor moves → preview scrolls to match
  - Backward SyncTeX: click preview → editor jumps to line
  - Chinese/English i18n
  - Configurable resolution (`defaultResolution`)
  - Configurable scroll speed (`scrollSpeed`)
  - Configurable max resolution cap (`maxResCap`)
  - Page input with i18n error dialog
  - One-click home/end/prev/next navigation (go-end supports cascading)
- **External window**: Retain original SDL window, Wayland/X11 env adaptation
- **Multi-file**: Sub-file → main-file mapping, bidirectional SyncTeX for `\include`/`\input`
- **Button visibility**: Built-in/external buttons mutually exclusive; SyncTeX buttons only visible during external preview

### Settings

| Key | Default | Description |
|-----|---------|-------------|
| `texpresso.cacheSize` | 10 | Preview page cache size |
| `texpresso.maxResCap` | 10 | Max resolution multiplier |
| `texpresso.defaultResolution` | 2.5 | Default resolution multiplier |
| `texpresso.scrollSpeed` | 0.3 | Scroll speed coefficient |

## File Changes

### texpresso C Backend

| File | Change | Description |
|------|--------|-------------|
| `src/frontend/webview_output.c` | **New** | QOI render output + dirty rect incremental diff |
| `src/frontend/webview_output.h` | **New** | Webview output interface declarations |
| `src/frontend/main.c` | Modified | Webview event loop, inline render, new command dispatch, polling advancement |
| `src/frontend/driver.c` | Modified | `-webview`/`-tmpdir`/`-resolution` flag parsing |
| `src/frontend/driver.h` | Modified | Webview-related state fields |
| `src/frontend/editor.c` | Modified | Parse 8 new commands |
| `src/frontend/editor.h` | Modified | New command tags and union fields |
| `src/frontend/renderer.c` | Modified | `invert_pixmap` made non-static, `render_to_pixmap` for offscreen rendering |
| `src/frontend/renderer.h` | Modified | Expose `invert_pixmap` and `render_to_pixmap` |

### texpresso-vscode Extension

| File | Change | Description |
|------|--------|-------------|
| `src/preview.ts` | **New** | PreviewPanel management class |
| `src/webview/index.ts` | **New** | Full Canvas viewer implementation |
| `src/extension.ts` | Modified | Command registration, multi-process, message routing, SyncTeX |
| `package.json` | Modified | New commands, buttons, settings, when conditions |
| `webpack.config.js` | Modified | Add webview webpack entry |

## Technical Highlights

### Incremental Rendering Pipeline

```
Source edit → texpresso incremental compile → MuPDF re-render current page
  → New pixmap vs old pixmap per-pixel comparison
  → Dirty area < 50%: each rect QOI encoded → page-diff message
  → Dirty area ≥ 50%: full page QOI → page message
```

### Real-time Editing Latency Optimization

- Keystroke changes → inline render (same iteration, no RELOAD_EVENT wait)
- RELOAD_EVENT deduplication guard (`webview_rendered_this_iteration` flag)
- Preloading phase does not trigger intermediate rendering

### Engine Polling

The TeX engine runs as a subprocess (texpresso-xetex) communicating via pipes. `step()` checks for pending queries with a 10μs timeout — returns false when the subprocess is computing but the engine is still `DOC_RUNNING`. Key loops (`go-end`, `set-page`) continuously poll rather than breaking on the first `false`.

## Backward Compatibility

- Without `-webview` flag, all SDL behavior is completely unchanged
- All existing commands (both S-expression and JSON protocol) are unaffected
- Existing VSCode commands have no breaking changes

## Improvement Suggestions

### Latency

1. **First-frame render timing**: Page 0 is currently rendered via RELOAD_EVENT after `advance_engine` discovers it. For complex documents, consider synchronous rendering as soon as page 0 is discovered, bypassing the event loop delay.

2. **Base64 overhead**: Encoding/decoding base64 for a full page QOI (~2.5MB @ 2.5x) takes ~6-8ms. VSCode's `postMessage` doesn't support ArrayBuffer transfer yet — if/when it does, this removes the 33% bloat and encoding cost.

### Performance

3. **Resolution-aware dirty rect detection**: Current per-pixel comparison runs at full resolution (e.g., 2.5x), scaling quadratically. Consider a quick low-resolution change check first, only upgrading to full resolution when changes are detected.

4. **Web Worker QOI decoding**: QOI decoding for large images (full page @ 2.5x, 8-15ms) could move to a Web Worker to avoid blocking the main thread.

5. **Canvas layering**: Page rendering and SyncTeX highlights share one canvas. Layering avoids full-page redraws.

6. **LRU cache pre-warming**: Pre-decode adjacent pages' QOI data during idle time (without rendering, just caching ImageBitmaps) to accelerate page turn.

### UX

7. **Pinch-to-zoom**: Current zoom requires Ctrl+wheel. Adding `gesturechange` event support improves trackpad experience.

8. **Smooth zoom animation**: Current zoom is instant. CSS transitions or Canvas inter-frame interpolation would make it feel smoother.

9. **Dark mode CSS inversion for images**: `\includegraphics` content is distorted by CSS `invert(1) hue-rotate(180deg)`. Consider C-side color inversion via `txp_renderer_invert_pixmap` instead, or let users choose.

10. **Mini-map**: For multi-page documents, a sidebar page thumbnail view would aid navigation.

11. **Expose keyboard shortcuts**: Shortcuts are currently hardcoded in the webview. Exposing them via VSCode keybinding contributions would let users customize.

### Robustness

12. **Temp file cleanup**: texpresso cleans QOI temp files on clean exit, but crashes may leave orphans. Add a periodic cleanup for stale `/dev/shm/texpresso-*` files (>1 hour old).

13. **Connection recovery**: When the texpresso process exits unexpectedly, show an error in the webview with a "reconnect" button instead of a silent black screen.

14. **Large document memory management**: Monitor total `pageCache` size and evict entries under memory pressure.

### Feature Extensions

15. **Forward SyncTeX highlight**: Show a blinking cursor or highlight on the preview at the position corresponding to the source cursor.

16. **Inline error display**: Overlay TeX compilation errors on the preview canvas at the relevant positions (like IDE red squiggles).

17. **TikZ preview enhancement**: Provide separate thumbnail previews for TikZ pictures.
