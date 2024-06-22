# Next version

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

# v0.0 Fri Apr  5 06:52:52 JST 2024

First publicly announced version of the project.
Still alpha.
