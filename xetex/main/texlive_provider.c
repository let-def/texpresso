#include "texlive_provider.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG 0

/// Structure representing a single hash table cell.
struct cell
{
  unsigned long hash;  ///< Hash value of the key.
  unsigned long offset;  ///< Offset in the entries buffer.
};

/// Structure representing a hash table.
struct table
{
  /// Hash table details.
  struct
  {
    int pow;  ///< Power of 2 for table size.
    int count;  ///< Number of entries.
                /// Load fill = count / (1 << pow) is less then 0.75.
    struct cell *cells;  ///< Array of cells.
  } hash;

  /// Entries buffer details.
  struct
  {
    char *buffer;  ///< Buffer to store key strings.
    int cap;  ///< Capacity of the buffer.
    int len;  ///< Length of used space in the buffer.
  } entries;

} table;

/// Calculates hash value for a given key using sdbm algorithm.
/// @param p Pointer to the key string.
/// @return Calculated hash value.
static unsigned long sdbm_hash(const void *p)
{
  unsigned long hash = 0;
  int c;

  for (const unsigned char *str = p; (c = *str); str++)
    hash = c + (hash << 6) + (hash << 16) - hash;

  return hash * 2654435761;
}

/// Calculates the number of cells in the hash table.
/// @param t Pointer to the table structure.
/// @return Number of cells.
#define cell_count(t) (1 << (t)->hash.pow)

/// Initializes the table structure.
/// @param table Pointer to the table structure.
static void init(struct table *table)
{
  table->hash.pow = 18;
  table->hash.count = 0;
  table->hash.cells = calloc(sizeof(struct cell), cell_count(table));
  table->entries.cap = 256;
  table->entries.len = 1;
  table->entries.buffer = malloc(256);
  table->entries.buffer[0] = '\0';
}

/// Cleans up and frees the memory allocated for the table.
/// @param table Pointer to the table structure.
static void finalize(struct table *table)
{
  if (table->hash.cells == NULL || table->entries.buffer == NULL)
    abort();
  free(table->hash.cells);
  free(table->entries.buffer);
  table->hash.cells = NULL;
  table->entries.buffer = NULL;
}

/// Looks up a cell in the hash table for a given key.
/// @param table Pointer to the table structure.
/// @param key Key string to look up.
/// @return Pointer to the cell.
///         If the key is in the hashtable, the cell is filled.
///         If not, the cell is empty and can be filled with the key.
static struct cell *lookup(struct table *table, const char *key)
{
  unsigned long hash = sdbm_hash(key);
  unsigned long mask = (1 << table->hash.pow) - 1;
  unsigned index = hash & mask;
  struct cell *cells = table->hash.cells;
  char *buffer = table->entries.buffer;
  while (cells[index].offset != 0)
  {
    if (cells[index].hash == hash &&
        strcmp(key, &buffer[cells[index].offset]) == 0)
      return &cells[index];
    index = (index + 1) & mask;
  }
  cells[index].hash = hash;
  return &cells[index];
}

/// Doubles the size of the hash table and rehashes all entries.
/// @param table Pointer to the table structure.
static void grow(struct table *table)
{
  if (LOG)
    printf("growing table\n");
  struct cell *ocells = table->hash.cells;
  int count = cell_count(table);

  table->hash.pow += 1;
  table->hash.cells = calloc(sizeof(struct cell), cell_count(table));
  if (!table->hash.cells)
    abort();

  for (int i = 0; i < count; i++)
  {
    struct cell *ocell = &ocells[i];
    if (ocell->offset != 0)
    {
      struct cell *ncell = lookup(table, &table->entries.buffer[ocell->offset]);
      if (ncell->offset != 0)
        abort();
      *ncell = *ocell;
    }
  }
  free(ocells);
}

/// Finds the path of a given file in the hash table.
/// @param table Pointer to the table structure.
/// @param key File name to look up.
/// @return Path of the file if found, otherwise NULL.
static const char *find(struct table *table, const char *key)
{
  struct cell *c = lookup(table, key);
  if (c->offset == 0)
  {
    if (LOG)
      printf("find: %s not found\n", key);
    return NULL;
  }
  const char *p = &table->entries.buffer[c->offset];
  while (p[-1] != '\0')
    p--;
  if (LOG)
    printf("find: %s found\n", key);
  return p;
}

/// Adds a file to the hash table.
/// @param table Pointer to the table structure.
/// @param root Root directory of the file.
/// @param dir Directory of the file.
/// @param name Name of the file.
static void add(struct table *table,
                const char *root,
                const char *dir,
                const char *name)
{
  struct cell *c = lookup(table, name);
  if (c->offset != 0)
  {
    char *p = &table->entries.buffer[c->offset];
    while (p[-1])
      p -= 1;
    if (LOG)
      printf("add: skipping %s/%s/%s\n  (already having: %s)\n", root, dir,
             name, p);
    return;
  }
  if (LOG)
    printf("add: adding %s\n", name);

  int rlen = strlen(root);
  int dlen = strlen(dir);
  int nlen = strlen(name);

  int tlen = table->entries.len + rlen + 1 + dlen + 1 + nlen + 1;

  if (table->entries.cap < tlen)
  {
    if (LOG)
      printf("add: growing buffer\n");
    while (table->entries.cap < tlen)
      table->entries.cap *= 2;
    table->entries.buffer = realloc(table->entries.buffer, table->entries.cap);
    if (!table->entries.buffer)
      abort();
  }

  char *data = table->entries.buffer + table->entries.len;
  memmove(data, root, rlen);
  data += rlen;
  *data++ = '/';
  memmove(data, dir, dlen);
  data += dlen;
  *data++ = '/';
  memmove(data, name, nlen);
  data[nlen] = 0;

  c->offset = data - table->entries.buffer;
  table->entries.len = tlen;

  table->hash.count += 1;
  if (table->hash.count * 3 == cell_count(table) * 2)
    grow(table);
}

/// Processes a single line from an "ls-R" file, adding entries to the hash
/// table.
/// @param table Pointer to the table structure.
/// @param path Path to the file to process.
static void process_line(struct table *table, char *path)
{
  FILE *f = fopen(path, "rb");

  if (!f)
  {
    perror("Cannot open file");
    printf("File: %s\n", path);
    return;
  }

  // Convert file path to directory path by replacing last '/' with '\0'
  char *slash = NULL;
  for (char *p = path; *p; p++)
    if (*p == '/')
      slash = p;

  if (slash)
    *slash = 0;

  char *sub = NULL;
  char *line = NULL;

  size_t cap = 0;
  ssize_t len;

  // Read each line from the file
  while ((len = getline(&line, &cap, f)) != -1)
  {
    if (len == 0)
      continue;

    if (line[len - 1] == '\n')
    {
      line[len - 1] = '\0';
      len--;
    }

    if (len == 0)
      continue;

    // Handle subdirectory specification
    if (line[0] == '.' && line[len - 1] == ':')
    {
      if (line[len - 2] == '/')
        line[len - 2] = 0;
      else
        line[len - 1] = 0;
      free(sub);
      sub = strdup(line[1] == '/' ? line + 2 : line + 1);
      continue;
    }

    // Add file to the table
    add(table, path, sub ? sub : "", line);
  }

  free(sub);
  free(line);

  if (fclose(f) != 0)
  {
    perror("fclose");
    return;
  }
}

struct table table;

/// Populate `table` with all TeX Live files by parsing the output of
///   `kpsewhich --all -engine=xetex ls-R`.
/// @return `true` if successful, `false` otherwise.
static bool list_texlive_files(void)
{
  static int loaded = 0;

  if (loaded)
    return (loaded == 1);

  loaded = -1;

  init(&table);

  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  int ret = 0;

  FILE *p = popen("kpsewhich --all -engine=xetex ls-R", "r");
  if (!p)
  {
    perror("popen");
    return 0;
  }

  while ((len = getline(&line, &cap, p)) != -1)
  {
    printf("Retrieved line of length %zd:\n", len);
    fwrite(line, len, 1, stdout);
    // Remove newline character from the end of the line
    if (len > 0 && line[len - 1] == '\n')
      line[len - 1] = '\0';
    process_line(&table, line);
  }

  free(line);

  ret = pclose(p);
  if (ret == -1)
    perror("pclose");
  else if (ret > 0)
    printf("Exit code: %d\n", ret);
  else
    loaded = 1;

  return (loaded == 1);
}

/// Retrieves the file size and modification time for a given path.
/// size and mtime are set to -1 if file does not exists.
///
/// @param path The path to the file.
/// @param size Pointer to store the file size.
/// @param mtime Pointer to store the modification time.
static void stat_path(const char *path, int *size, int *mtime)
{
  struct stat st;
  if (path && stat(path, &st) == 0)
  {
    *size = st.st_size;
    *mtime = st.st_mtime;
  }
  else
  {
    *size = -1;
    *mtime = -1;
  }
}

/// Finds the path of a TeX Live file and optionally records its dependency.
/// @param name Name of the file to find.
/// @param record_dependency File pointer to record dependency information.
/// @return The path to the file.
const char *texlive_file_path(const char *name, FILE *record_dependency)
{
  list_texlive_files();
  const char *path = find(&table, name);

  if (record_dependency)
  {
    int size, mtime;
    stat_path(path, &size, &mtime);
    fprintf(record_dependency, "%s\n%d:%d\n", name, size, mtime);
  }
  return path;
}

/// Checks if the recorded dependencies are still valid.
/// @param record File pointer containing recorded dependencies.
/// @return `true` if all dependencies are valid, `false` otherwise.
bool texlive_check_dependencies(FILE *record)
{
  char name[1024];
  int size, mtime;

  while (fscanf(record, "%1023[^\n]\n%d:%d\n", name, &size, &mtime) == 3)
  {
    int size2, mtime2;
    stat_path(find(&table, name), &size2, &mtime2);
    if (size != size2 || mtime != mtime2)
      return 0;
  }
  return 1;
}

/// Lists all available texlive files.
/// @return 1 if successful, 0 otherwise.
bool texlive_available(void)
{
  return (list_texlive_files() == 1);
}
