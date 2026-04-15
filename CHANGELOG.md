# NEXT

- fix TeX build date to use SOURCE_DATE_EPOCH if set, otherwise falling back to
  the current date (previously hardcoded to Feb 8, 2025).
- fix zealous JSON escaping
- add `(open-base64)` command for binary data (@merv1n34k)
- add `-stream` flag for filesystem-independent editing (@merv1n34k)
- fix provider auto-detection short-circuit logic (@alvv-z)

# v0.2 Fri  6 Mar 19:09:31 JST 2026

Finally, the engine is independent of tectonic and the build does not need rust anymore:

- fix JSON generated for input-file message (@DominikPeters)
- support UTF-16 surrogates in `(change-range)`
- sanitize JSON string serialization
- embed a custom fork of XeTeX engine (now independent of tectonic)
- texpresso can now read packages either from TeXlive or Tectonic installations
  (default to TeXlive if present, pass `-tectonic` argument to switch to Tectonic)
- CI for multiple platforms contributed by @merv1n34k

# v0.1 Mon 11 Aug 14:42:38 JST 2025

Hopefully the last release still relying on a custom build of tectonic:

- Re-enable SyncTeX (accidentally disabled during a rebase)
- Fix a crash when the same file was opened multiple times
- Disable compositor bypass (#50, report by @adamkruszewski)
- Handle infinite loops in TeX code
- Fix race conditions when latest process observe data being changed
- Workaround limitations of fork on macOS when using system fonts
- Up/down scrolls vertically through the document, and change pages when reaching the border (#52, contributed by @gasche)
- Fix JSON printing of non-ASCII characters (broke paths with chinese characters, see #53)
- Fix a crash due to a broken invariant when the contents being edited has been read by the TeX worker but the driver is not aware (#62)
- Don't require re2c for a non-developer build (#64)
- Load local font/resources files from filesystem
- Remove gumbo dependencies on macOS (change in homebrew packaging of mupdf)
- On Linux, try Wayland video driver first to solve HIDPI issues
- PDF engine: reload document on filesystem changes
- Add `(input-file)` message to let editor track source files (#
- Add `(change-range)` command to support change specified as UTF-16 code unit ranges, following LSP protocol (with help from @lnay)
- New logo (contributed by @merv1n34k)
- Build fixes for different platforms (various contributors)
- PDF: fix parsing of indirect references, improve support for dashes in stroking operators (reported by @gasche)

# v0.0 Fri Apr  5 06:52:52 JST 2024

First publicly announced version of the project.
Still alpha.
