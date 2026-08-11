Test documents and scripts.

Run everything with `make test-report` from the repo root: it runs each target
below, prints PASS/FAIL with timings, and exits non-zero if any failed. Run a
single target to see why one failed.

## Documents

[simple.tex](simple.tex): sample .tex file to check that basic features work.
Used by `make test-texpresso`, its `-texlive` / `-tectonic` variants, and
`make test-open-base64`.

[include.tex](include.tex): tests include-path support; needs `-I incpath`
passed to TeXpresso to build correctly.

[missing-input.tex](missing-input.tex): `\input`s a file that does not exist, so
the engine reports the miss and the editor can supply it. Used by
`make test-lookup-file` and `make test-register`.

[refs.tex](refs.tex): cross-references, for aux-driven convergence.

[format.tex](format.tex), [includegraphics.tex](includegraphics.tex): used
manually / by `make fill-tectonic-cache`.

## Scripts

| target | script | checks |
|---|---|---|
| `test-stream` | `test_stream.sh` | the `-stream` command channel |
| `test-register` | `test-register.sh` | `(register)` then `lookup-file promised` then `(open)` resumes the run |
| `test-lookup-file` | `test-lookup-file.sh` | an unregistered miss is reported and the supplied file is used |
| `test-rerun` | `test-rerun.sh` | aux-driven convergence (TOC / refs) across reruns |
| `test-replay` | `test-replay.sh` | incremental replay is byte-identical to a full compile |
| `test-fence` | `test-fence.sh` | snapshot restore reproduces linear memory exactly, against the standalone engine |

`test-replay` edits at 1/10/50/90% of the document. The 1% position is
deliberate: it is the one that selects the base checkpoint. Sampling only the
middle of the document hid a bug that produced no output at all for edits near
the top.
