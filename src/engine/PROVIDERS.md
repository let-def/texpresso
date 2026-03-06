# TeXpresso Providers

TeXpresso embeds its own version of the XeTeX engine but needs a TeX
distribution to run it. It currently supports two distribution "providers"
which are used to load files from existing distributions: `tectonic` and
`texlive`.

A provider fills two roles:
- The primary one is to return the contents of a file from its name (e.g., when
  one does `\usepackage{foo}`, the engine will search for `foo.sty`; if it is
  not contained in the input directory, the provider will be asked for this
  file).
- The second one is to provide a way to detect upgrades in order to invalidate
  files derived from the distribution (this is relevant for _format_ files).

## Design of the Tectonic Provider

Interactions with a Tectonic installation happen through the `tectonic` binary.
TeXpresso will consider that Tectonic is available only if `tectonic` can be
executed.

Tectonic has not really been designed with the idea of being used as a provider
of TeX dependencies, but two functions are offered that enable this use:
- `tectonic -X bundle search` lists the names of all files it can provide.
- `tectonic -X bundle cat <name>` outputs the contents of file `<name>`
  (if it has it).

Bundle is the name given by Tectonic to a snapshot of all TeX packages it
provides. `tectonic -X bundle cat SHA256SUM` returns a hash of the bundle,
which we use as a version identifier.

By default, files are fetched from the internet on demand and cached on disk.
The cache directory can be found using `tectonic -X show user-cache-dir`.

### Failure Paths

Commands can fail if the internet connection is not available:
1. If Tectonic has never been used, it has to fetch a bundle index first. In
   this case, `bundle search` can fail. `bundle search` also checks for updates
   from time to time by fetching upstream `SHA256SUM`. This can also fail if
   internet is not available.
2. If Tectonic has been properly initialized, `bundle search` can succeed, and
   we could think a specific file is available, yet `bundle cat` can fail
   because the file is not in cache and cannot be downloaded.

### Integration with TeXpresso

The main challenge for TeXpresso is that using `tectonic -X bundle cat` is too
slow. A process has to be spawned for each request, and the initialization costs
add up quickly.

So TeXpresso maintains its own cache in the user-wide cache directory,
`CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}`. TeXpresso-specific files go into
`$CACHE_DIR/texpresso`, and Tectonic ones to `$CACHE_DIR/texpresso/tectonic`.
The latter only stores a copy of `bundle cat` contents: if
`$CACHE_DIR/texpresso/tectonic/foo` exists, it should have the same contents as
`tectonic -X bundle cat foo`.

#### Initialization

The provider starts by registering all names in `tectonic -X bundle search` in a
hash set. Then it creates the cache directory if necessary and checks if
`SHA256SUM` is the same as `tectonic -X bundle cat SHA256SUM`. If not, it clears
the contents of the cache.

#### Requesting a File

Whenever a file is requested:

1. **Check Hash Set**: If the file is not in the hash set, the request is
   declined immediately.
2. **Check Cache**: If it is in the set and in cache, its contents are returned.
3. **Fetch and Cache**: If not, `bundle cat` is used to fetch the file:
   - The fetched file is copied to the cache.
   - The contents are sent to the XeTeX engine.

## Design of the TeXlive Provider

TeXpresso uses the `kpsewhich` binary to interact with a TeXlive installation.
TeXpresso will consider that TeXlive is available only if the command
`kpsewhich -engine=xetex --all ls-R` succeeds.

When successful, it outputs a list of absolute file paths of files that contain
lists of relative file paths in the format of `ls -R`.

### Integration with TeXpresso

The `ls-R` lists are read by TeXpresso and used to populate a map from file
names to absolute paths. File requests are answered by looking up this map.

To detect version changes, requests can be recorded:
- Whether they succeeded or failed.
- If they succeeded, the mtime and size of the target are recorded too.

### Limitations
- FIXME: Error handling and logging.
- BY DESIGN: Approximation of actual visibility and file ranking for a given
  engine

## Generation of Formats
