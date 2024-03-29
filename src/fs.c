/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <string.h>
#include "state.h"
#include "mupdf_compat.h"
#include "dvi/fz_util.h"

static unsigned long
sdbm_hash(const unsigned char *str)
{
  unsigned long hash = 0;
  int c;

  while ((c = *str++))
    hash = c + (hash << 6) + (hash << 16) - hash;

  return hash * 2654435761;
}

#define str_hash sdbm_hash

typedef struct
{
  unsigned long hash;
  fileentry_t *entry;
} tablecell;

struct filesystem_s
{
  int count, cap;
  tablecell *table;
};

static const char *normalize_path(const char *path)
{
  if (path[0] == '.' && path[1] == '/')
  {
    path += 2;
    while (*path == '/')
      path += 1;
  }
  return path;
}

filesystem_t *filesystem_new(fz_context *ctx)
{
  fz_ptr(filesystem_t, fs);
  fz_ptr(tablecell, table);

  fz_try(ctx)
  {
    fs = fz_malloc_struct(ctx, filesystem_t);
    fs->cap = 64;
    table = fz_malloc_struct_array(ctx, 64, tablecell);
    fs->table = table;
  }
  fz_catch(ctx)
  {
    if (table)
      fz_free(ctx, table);
    if (fs)
      fz_free(ctx, fs);
    fz_rethrow(ctx);
  }
  return fs;
}

void filesystem_free(fz_context *ctx, filesystem_t *fs)
{
  int cap = fs->cap;
  for (int i = 0; i < cap; ++i)
  {
    fileentry_t *e = fs->table[i].entry;
    if (!e)
      continue;
    if (e->fs_data)
      fz_drop_buffer(ctx, e->fs_data);
    if (e->edit_data)
      fz_drop_buffer(ctx, e->edit_data);
    if (e->saved.data)
      fz_drop_buffer(ctx, e->saved.data);
    fz_free(ctx, (void *)e->path);
    fz_free(ctx, fs->table[i].entry);
  }
  fz_free(ctx, fs->table);
  fz_free(ctx, fs);
}

static tablecell *table_get(int cap, tablecell *table, const char *path)
{
  unsigned long mask = cap - 1;
  unsigned long hash = str_hash((const unsigned char*)path);

  int index = hash & mask;

  while (table[index].entry)
  {
    if (table[index].hash == hash && strcmp(table[index].entry->path, path) == 0)
      break;
    index = (index + 1) & mask;
  }
  table[index].hash = hash;
  return &table[index];
}

static tablecell *table_resize(fz_context *ctx, int oldcap, tablecell *oldtab, int newcap)
{
  tablecell *newtab = fz_malloc_struct_array(ctx, newcap, tablecell);
  int mask = newcap - 1;
  for (int i = 0; i < oldcap; ++i)
  {
    if (!oldtab[i].entry)
      continue;
    tablecell cell = oldtab[i];
    int index = cell.hash & mask;
    while (newtab[index].entry)
    {
      if ((cell.hash & mask) < (newtab[index].hash & mask))
      {
        tablecell tmp = newtab[index];
        newtab[index] = cell;
        cell = tmp;
      }
      index = (index + 1) & mask;
    }
    newtab[index] = cell;
  }
  return newtab;
}

fileentry_t *filesystem_lookup(filesystem_t *fs, const char *path)
{
  return table_get(fs->cap, fs->table, normalize_path(path))->entry;
}

fileentry_t *filesystem_lookup_or_create(fz_context *ctx, filesystem_t *fs, const char *path)
{
  path = normalize_path(path);
  tablecell *cell = table_get(fs->cap, fs->table, path);
  fileentry_t *entry = cell->entry;

  if (entry != NULL) return entry;

  entry = fz_malloc_struct(ctx, fileentry_t);
  entry->path = fz_strdup(ctx, path);
  entry->saved.level = FILE_NONE;
  entry->seen = -1;
  entry->pic_cache.type = -1;
  entry->fs_stat.st_ino = 0;
  cell->entry = entry;

  fs->count += 1;
  if (fs->count * 4 >= fs->cap * 3)
  {
    int newcap = fs->cap * 2;
    tablecell *newtab = table_resize(ctx, fs->cap, fs->table, newcap);
    fz_free(ctx, fs->table);
    fs->cap = newcap;
    fs->table = newtab;
  }

  return entry;
}

fileentry_t *filesystem_scan(filesystem_t *fs, int *index)
{
  while (*index < fs->cap)
  {
    int i = *index;
    *index += 1;
    if (fs->table[i].entry)
      return fs->table[i].entry;
  }
  return NULL;
}
