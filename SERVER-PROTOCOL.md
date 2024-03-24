# Client-server protocol

The server (TeXpresso) coordinates clients (Tectonic/TeX) to render the document. The server has control over all processes, but it is the clients that do the actual work.

The many "clients" are successive forks of a root process. 

The server spawns this root client and use a fresh unix domain socket for bidirectional communication.

Implementations:
- C server in files [sprotocol.h](src/sprotocol.h) and [sprotocol.c](src/sprotocol.c)
- Rust client in [texpresso\_protocol](tectonic/crates/texpresso_protocol/src/lib.rs) crate

## Communication

First, the client writes `TEXPRESSOC01` and the server writes `TEXPRESSOS01`.

Then the server reads `TEXPRESSOC01` and the client reads `TEXPRESSOS01`. If any of these fails, the client aborts or is killed by the server.

After that, the protocol is a serie of questions and answers mostly driven by the client with occasional requests from the server. 

Questions and answers starts with a four letter identifier.

Client queries:

- `OPEN(PATH: TEXT, MODE: TEXT, ID: FILEID) -> DONE | PASS`

  Client wants open a file for reading or writing.

- `READ(FILE: FILEID, SIZE: INT) -> READ(BUF: BYTES) | FORK`

  Client wants to read from a file that has been opened for reading (if not it
  is an offense worth killing). The server can either satisfy the read
  (potentially with less bytes than requested) or ask the client to fork.
  The bytes that are sent to the client are not yet considered as "observed" by
  the server. The server considers that a byte has been observed only when the
  client acknowledge with a `SEEN` message (see below).
  This is necessary to buffer contents (avoiding some communication overhead)
  while enabling fine-grained tracking of the process state.

- `WRIT(FILE: FILEID, BUF: BYTES) -> DONE`

  Client wants to write to a file opened from writing.

- `CLOS(FILE: FILEID) -> DONE`

  Clients no longer need the give file, it can be closed.

- `SEEK(FILE: FILEID, POS: INT) -> DONE`

  Clients want to seek to a given position in a file (opened for reading or writing).

- `SEEN(FILE: FILEID, POS: INT)` 

  Notify the server that the content up to the given position has been observed by the process.
  There is no answer, it is just a notification to the server.
  This is the only way for the server to know that a client has observed some contents.

- `CHLD(PID: INT; AUXILIARY FD) -> DONE`
  The client forked, the argument is the pid of the new child.
  The file descriptor to use to communicate with the new child is 

- `SPIC(PATH: TEXT, TYPE: INT, PAGE: INT, BOUNDS: FLOAT[4]) -> DONE`
  "Store pic [boundaries]". For performance reason, this is used to cache the
  dimension of a picture included in a LaTeX document.
  The serveur maintains a hashtable storing boundaries, expressed as 4 floats,
  and indexed by path, type and page.
  This infrastructure often allow to skip rescanning JPG/PNG/PDF, significantly increasing performance during incremental changes. FIXME: only path is used as an index, the pair (type, page) is stored in a single cell cache

- `GPIC(PATH: TEXT, TYPE: INT, PAGE: INT) -> GPIC(BOUNDS: FLOAT[4]) | PASS`
  "Get pic [boundaries]". If the file as path has not changed and is queried for a type and page that was previously stored using `SPIC`, the previous boundaries should be returned; it is always safe to `PASS`, but that can affect the performance.
  

Server queries:

- `FLSH`
  The client should flush (invalidate) all its read buffers; their content might be out of date. No answer is expected.

Whenever the client wants to send a query or read an answer, it should first check for any outstanding query from the server. 

When the server no longer needs a client, it is simply killed by sending a TERM signal.
