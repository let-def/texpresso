# Controlling TeXpresso from an editor

The process should be started from the editor passing the root TeX file as argument:

```
texpressso [-json] <some-dir>/root.tex
```

The rest of the communication will happen on stdin/stdout:
- TeXpresso reads and interprets commands found on stdin
- when it needs to communicate some events, it writes a message on stdout.

### General considerations

### Best effort and asynchronous protocol

It is not a query/answer protocol (e.g. request something on stdin, read the answer from stdout): stdin and stdout are processed independently.

The editor should just write a line on stdin when it needs to communicate something to TeXpresso. 

Conversely, it can interpret the messages it is interested in; ignoring the others should be safe.

### Syntax

The protocol is designed around s-expressions (each command and message is represented by one s-exp value).

**TODO**: For compatibility with other ecosystems, minimal json support is planned. When started with the `-json` argument, TeXpresso will switch its communication to a json-compatible format: s-expression lists will simply be replaced by JSON arrays (`(a "b" 12)` -> `["a", "b", 12]`).

Later, the protocol could be updated to be more JSON-friendly. In particular, right now it distinguishes between symbols/names (`a`) and strings (`"a"`), a subtlely that exists both in PDF value format and s-expressions, but is foreign to JSON afaict.

### VFS

An important part of the protocol is communicating the contents of a "virtual file system" to TeXpresso. This "VFS" is made of the buffers opened in the editor, which contents might have not been saved to disk yet.
When looking for a file, TeXpresso checks in its VFS first and then fallbacks to disk. The editor should try to keep TeXpresso view synchronized. Emacs mode does this by listening for changes and sending deltas to TeXpresso.

### Strings, codepoints and bytes

LaTeX consumes files as a stream of bytes (8-bit integers). TeXpresso makes no assumption on the encoding, it just shares the raw contents stored on disk.

However, this might not be the case for the content communicated by the editor. 
JSON strings, for instance, are serialized sequence of unicode codepoints. TeXpresso will convert it to an UTF-8 encoded byte string before sharing with LaTeX. Offsets specified in delta commands should therefore refer to byte offsets of the UTF-8 representation.

**TODO**: It might be possible to switch to a more convenient convention. For instance using line-based deltas, using line numbers rather than byte offsets might be more convenient for neovim.

## Commands (editor -> texpresso)

```scheme
(open "path" "contents")
```

Populate TeXpresso VFS with a file at "path" storing "contents".


```scheme
(close "path")
```

Remove file at "path" from TeXpresso VFS.

```scheme
(change "path" offset length "data")
```

Update file at "path" in VFS (it should have been `open`ed before), by replacing `length` bytes starting at `offset` (both are integers) with the contents of "data".

```scheme
(theme (bg_r bg_g bg_b) (fg_r fg_g fg_b))
```

Synchronize the theme with the one of the editor, to display the document with the same background and foreground colors.
Values `bg_r`, `bg_g`, ... are floating point values in `[0.0, 1.0]` interval.

```scheme
(previous-page)
(next-page)
```

Display previous or the next page of the document. It is convenient to bind these to some shortcuts to change pages without leaving the editor (in Emacs this is done via the `(texpresso-{previous,next}-page)` interactive commands).

```scheme
(move-window x y w h)
```

Move TeXpresso to the specified screen coordinates. For GUI editors, this can be convenient to keep TeXpresso positioned relative to the main window.

```scheme
(stay-on-top t)
(stay-on-top nil)
```

Asking the window manager to keep TeXpresso window above the others, or not. This can be convenient to keep a TeXpresso window floating on top of the editor. (`t` and `nil` are the closest approximation of "true" and "false" in emacs-sexp).

## Messages (texpresso -> editor)

### Synchronizing output messages and log file

```
(truncate out|log size)
(append out|log text)
(flush)
```

These three commands are used to share the contents of LaTeX output messages and logs in real-time.
During a LaTeX run, these are append-only emitting only `append` commands. When a change happens in the document, the process might backtrack, sending a `truncate` message to drop the invalidated parts of the output.
Size is expressed as a number of byte.

Because this can happen a lot during edition, it is a good practice to buffer these changes and only reflect from time to time. TeXpresso will signal moments it deemed appropriate with a `flush` message.
However this is not always sufficient to provide a pleasant experience. See also [texpresso#10](https://github.com/let-def/texpresso/issues/10): it is planned to extend this API to provide a better experience while simplifying the logic on the editor side.

Sample script:
```
(truncate out 0)
(truncate log 0)
(append out "foo.tex:12: overfull hbox")
(append log "This is XeTeX...")
(flush)
```


### SyncTeX

```
(synctex "path" line)
```

SyncTeX backward synchronisation: the user clicked on text produced by LaTeX sources at path:line. The action is usually to open this file in the editor and jumps to this line.


### VFS reset

```
(reset-sync)
```

Output by TeXpresso when the contents of its VFS has been lost. The editor should re-`open` any file before sharing `change`s.
Not urgent: this notification is used mainly when debugging TeXpresso, it should not happen during normal use.
