#include "texlive_provider.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG 0

struct cell
{
  unsigned long hash, offset;
};

struct table
{
  struct
  {
    int pow;
    int count;
    struct cell *cells;
  } hash;
  struct
  {
    char *buffer;
    int cap;
    int len;
  } entries;

} table;

static unsigned long sdbm_hash(const void *p)
{
  unsigned long hash = 0;
  int c;

  for (const unsigned char *str = p; (c = *str); str++)
    hash = c + (hash << 6) + (hash << 16) - hash;

  return hash * 2654435761;
}

#define cell_count(t) (1 << (t)->hash.pow)

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

static void finalize(struct table *table)
{
  if (table->hash.cells == NULL || table->entries.buffer == NULL)
    abort();
  free(table->hash.cells);
  free(table->entries.buffer);
  table->hash.cells = NULL;
  table->entries.buffer = NULL;
}

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

static void process_line(struct table *table, char *path)
{
  FILE *f = fopen(path, "rb");

  if (!f)
  {
    perror("Cannot open file");
    printf("File: %s\n", path);
    return;
  }

  char *slash = NULL;
  for (char *p = path; *p; p++)
    if (*p == '/')
      slash = p;

  if (slash)
    *slash = 0;
  // file is now the directory

  char *sub = NULL;
  char *line = NULL;

  size_t cap = 0;
  ssize_t len;

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

static void list_texlive_files(void)
{
  static int loaded = 0;
  if (loaded)
    return;
  loaded = 1;

  init(&table);

  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  int ret = 0;

  FILE *p = popen("kpsewhich --all -engine=xetex ls-R", "r");
  if (!p)
  {
    perror("popen");
    return;
  }

  while ((len = getline(&line, &cap, p)) != -1)
  {
    printf("Retrieved line of length %zd:\n", len);
    fwrite(line, len, 1, stdout);
    if (len > 0 && line[len - 1] == '\n')
      line[len - 1] = '\0';
    process_line(&table, line);
  }

  free(line);

  ret = pclose(p);
  if (ret >= 0)
    printf("Exit code: %d\n", ret);
  else if (ret == -1)
    perror("pclose");
}

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
