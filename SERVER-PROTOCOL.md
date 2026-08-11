# Client-server protocol (removed)

TeXpresso used to spawn a patched TeX engine as a separate process, drive it
over a unix domain socket, and fork it to take snapshots. That protocol
(`OPEN`, `READ`, `WRIT`, `SEEN`, `CHLD` and the rest), its implementation in
`src/sprotocol.[ch]`, and the Rust client are all gone. They were removed in
`eac541590` and remain in the history.

The engine now runs inside the texpresso process: stock upstream sources
compiled to WebAssembly and then to C, with snapshots taken by userland
copy-on-write instead of `fork(2)`. No inter-process protocol is left to
document.

## See also

- `WASM-ENGINE.md`: how the engine is built, driven and snapshotted.
- `EDITOR-PROTOCOL.md`: the protocol between an editor and texpresso, which is
  unchanged.
