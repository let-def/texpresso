# Controlling TeXpresso from an editor

The process should be started from the editor passing the root TeX file as argument:

```
texpressso [-I path]* [-json] [-lines] <some-dir>/root.tex
```

The rest of the communication will happen on stdin/stdout:
- TeXpresso reads and interprets commands found on stdin
- when it needs to communicate some events, it writes a message on stdout.

Description of the arguments:
- `-json`: use a JSON syntax rather than SEXP syntax for communication
- `-lines`: update output buffers line-by-line rather than by chunks of bytes (using `append-lines`/`truncate-lines` rather than `append`/`truncate` messages)
- `-I path`: populate an "include path" in which files should be looked up in priority

The include path is useful if one uses a build system that puts auxiliary files in a dedicated build directory, while the TeX sources are in a separate source directory. In this case, TeXpresso can be started using `texpresso -I build/ source/main.tex`.

### General considerations

### Best effort and asynchronous protocol

It is not a query/answer protocol (e.g. request something on stdin, read the answer from stdout): stdin and stdout are processed independently.

The editor should just write a line on stdin when it needs to communicate something to TeXpresso. 

Conversely, it can interpret the messages it is interested in; ignoring the others should be safe.

### Syntax

The protocol is designed around s-expressions (each command and message is represented by one s-exp value).

JSON-mode (`-json` flag) prints sexp as follow:
- a sexp-list is represented by a JSON array
- a sexp atom (symbol) is represented by a JSON string
- special characters are escaped using JSON lexical conventions

### VFS

An important part of the protocol is communicating the contents of a "virtual file system" to TeXpresso. This "VFS" is made of the buffers opened in the editor, which contents might have not been saved to disk yet.
When looking for a file, TeXpresso checks in its VFS first and then fallbacks to disk. The editor should try to keep TeXpresso view synchronized. Emacs mode does this by listening for changes and sending deltas to TeXpresso.

### Strings, codepoints and bytes

LaTeX consumes files as a stream of bytes (8-bit integers). TeXpresso makes no assumption on the encoding, it just shares the raw contents stored on disk.

However, this might not be the case for the content communicated by the editor. 
JSON strings, for instance, are serialized sequences of unicode codepoints. TeXpresso will convert it to an UTF-8 encoded byte string before sharing with LaTeX.
Offsets specified in byte-based delta commands (`change`) should therefore refer to byte offsets of the UTF-8 representation.
Alternatively, one can use line-based communication (using `change-lines` for editor->TeXpresso communication, and by passing `-lines` argument for TeXpresso->editor messages). Lines are numbered by counting '\n'.

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
(change-lines "path" offset count "data")
```

Update file at "path" in VFS (it should have been `open`ed before), by replacing `length` lines starting from line number `offset` (both are integers) with the contents of "data".
Line numbering starts at 0 and line count is defined as the number of '\n'.
To insert multiple lines, separate them with '\n' in data. Note that if data ends with '\n', it will insert a new empty-line at the end.

```scheme
(change-range "path" start-line start-column end-line end-column "replacement-text")
```

Update file at "path" in VFS (it should have been `open`ed before), by replacing the characters in range starting from line `start-line` at column `start-column` (implemented by counting the number of UTF-16 code units, with 0 being the beginning of the line) up to line `end-line` at column `end-column`. This is designed to be compatible with [LSP position encoding](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#positionEncodingKind) using the default 'utf-16' encoding.

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
(rescan)
```

Check the filesystem for changes. This will reload and reprocess any changed file.

```scheme
(stay-on-top t)
(stay-on-top nil)
```

Asking the window manager to keep TeXpresso window above the others, or not. This can be convenient to keep a TeXpresso window floating on top of the editor. (`t` and `nil` are the closest approximation of "true" and "false" in emacs-sexp).

```scheme
(synctex-forward "path" line)
```

Try to scroll the UI to the contents defined in TeX file at "path" and line. The path can be absolute or relative to the root document.

## Messages (texpresso -> editor)

### Byte-based synchronization of output messages and log file

```
(truncate out|log size)
(append out|log text)
(flush)
```

These three commands are used to share the contents of LaTeX output messages and logs in real-time.
During a LaTeX run, these are append-only emitting only `append` commands. When a change happens in the document, the process might backtrack, sending a `truncate` message to drop the invalidated parts of the output.
Size is expressed as a number of byte.

Because this can happen a lot during edition, it is a good practice to buffer these changes and only reflect from time to time. TeXpresso will signal moments it deemed appropriate with a `flush` message.
Sample script:
```
(truncate out 0)
(truncate log 0)
(append out "foo.tex:12: overfull hbox")
(append log "This is XeTeX...")
(flush)
```

### Line-based synchronization of output messages and log file

```
(truncate-lines out|log count)
(append-lines out|log line1 line2... lineN)
(flush)
```

Like their byte-based counterparts, these commands are used to share the contents of LaTeX output messages and logs in real-time. They are used instead of the byte-based one when TeXpresso is started with `-lines` flag,

As the LaTeX process runs, it appends data to the standard output and to the log file.
All completed lines (separated by `\n`), TeXpresso communicates them to the editor. The `\n` are not included in the serialized strings.

When a change happens in the document, the process might backtrack, sending a `truncate-lines` message to drop the invalidated parts of the output. Only `count` lines should be kept.

Because this can happen a lot during edition, it is a good practice to buffer these changes and only reflect from time to time. TeXpresso will signal moments it deemed appropriate with a `flush` message. This is less of a problem with the line-based output.

For instance, to avoid flickering, an editor can keep stale lines after receiving a `truncate-lines` message, overwrite them as `append-lines` messages are received, and really truncate when receiving `flush` message. At this point, all invalidated contents as been removed but the transition has been smoothed out.

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

### Files used by the document

```
(input-file index "path")
```

Output by TeXpresso when a new source file is read by the document.

During a single run `index` is a unique, monotonous integer. When a new `input-file` message is produced with a lower index, it means that the process backtracked and all files with a higher index are no longer monitored. (Though this is very likely to be temporary, it is better to batch the changes and update the editor state only from time to time).

The paths are printed relative to the root file. They might be non-existent on the file-system (for instance if they exist only in TeXpresso's overlay, that is the case for the intermediate files produced by beamer), so editor plugins should not make any assumptions and validate the path themselves.

Right now, this is implemented by hooking into SyncTeX:
- only text files are tracked (not graphics)
- the indices printed are the SyncTex input indices; they should be attributed no other meaning than being monotonic and useful to detect backtracking occurrences
