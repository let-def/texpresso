# Next version

- Re-enable SyncTeX (accidentally disabled during a rebase)
- Fix a crash when the same file was opened multiple times
- Disable compositor bypass (#50, report by @adamkruszewski)
- Handle infinite loops in TeX code
- Fix race conditions when latest process observe data being changed
- Workaround limitations of fork on macOS when using system fonts
- Up/down scrolls vertically through the document, and change pages when reaching the border (#52, contributed by @gasche)
- Fix JSON printing of non-ASCII characters (broke paths with chinese characters, see #53)

# v0.0 Fri Apr  5 06:52:52 JST 2024

First publicly announced version of the project.
Still alpha.
