#include "common.h"
#include "providers.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READ_BUF_SIZE 4096
#define READ_HASH_SIZE 128
#define TECTONIC_PATH_MAX 4096
#define TECTONIC_CMD_MAX 1024

/* -------------------------------------------------------------------------- */
/* Global State (Dynamic buffers for index, dirs, entries)                    */
/* -------------------------------------------------------------------------- */

int *entries = NULL;
int entries_pow = 1;
int entries_cap = 1;

struct dynbuf
{
  char *data;
  size_t len;
  size_t cap;
} tt_index = {0, .cap = 1}, tt_dirs = {.cap = 1};

bool tt_index_skip = false;
bool tt_is_v15 = false;

/* Static buffers for paths, commands, and cache dir */
static char g_path_buf[TECTONIC_PATH_MAX];
static char tectonic_cache_dir[TECTONIC_PATH_MAX] = {0};
static bool tectonic_cache_init = false;

static void dynbuf_ensure_capacity(struct dynbuf *buf, size_t cap)
{
  if (cap < buf->cap)
    return;
  while (cap >= buf->cap)
    buf->cap *= 2;
  char *tmp = realloc(buf->data, buf->cap);
  if (!tmp)
    do_abortf("tectonic provider: cannot allocate dynamic buffer");
  buf->data = tmp;
}

/* -------------------------------------------------------------------------- */
/* Hash Table Utilities                                                       */
/* -------------------------------------------------------------------------- */

static unsigned long sdbm_hash(const char *p)
{
  unsigned long hash = 0;
  int c;
  for (const unsigned char *str = (const unsigned char *)p; (c = *str); str++)
    hash = c + (hash << 6) + (hash << 16) - hash;
  return hash * 2654435761;
}

static int lookup_entry_index(const char *name)
{
  unsigned long hash = sdbm_hash(name);
  int mask = (1 << entries_pow) - 1;
  int index = hash & mask;
  while (1)
  {
    int offset = entries[index];
    if (offset == -1)
      return index;
    if (strcmp(name, tt_index.data + offset) == 0)
      return index;
    index = (index + 1) & mask;
  }
}

static void build_hash_table(void)
{
  int count = 0;
  for (size_t i = 0; i < tt_index.len; i++)
    if (tt_index.data[i] == '\0')
      count++;

  int min_cap = count * 4 / 3;
  while ((1 << entries_pow) <= min_cap)
    entries_pow++;
  entries_cap = 1 << entries_pow;

  entries = realloc(entries, entries_cap * sizeof(int));
  if (!entries)
    do_abortf("Cannot allocate table for indexing Tectonic bundle.");
  memset(entries, -1, entries_cap * sizeof(int));

  for (size_t i = 0, j = 0; j < tt_index.len; j++)
  {
    if (tt_index.data[j] != '\0')
      continue;
    if (i != j)
    {
      int index = lookup_entry_index(tt_index.data + i);
      if (entries[index] == -1)
        entries[index] = (int)i;
      else
        fprintf(stderr, "tectonic bundle: duplicate entry (%s)\n",
                tt_index.data + i);
    }
    i = j + 1;
  }

  fprintf(stderr, "tectonic bundle: indexing succeeded (%d entries)\n", count);
}

/* -------------------------------------------------------------------------- */
/* Path Building (Macro + Static Buffer Mutation)                             */
/* -------------------------------------------------------------------------- */

static const char *tectonic_user_cache_path_impl(const char *suffixes[])
{
  if (!tectonic_cache_init)
  {
    tectonic_cache_init = true;
    FILE *p = popen("tectonic -X show user-cache-dir", "r");
    if (!p)
    {
      fprintf(stderr, "tectonic: not found\n");
      return NULL;
    }
    size_t n = fread(tectonic_cache_dir, 1, sizeof(tectonic_cache_dir) - 1, p);
    pclose(p);
    if (n > 0 && tectonic_cache_dir[n - 1] == '\n')
    {
      n--;
      tectonic_cache_dir[n] = '\0';
      fprintf(stderr, "tectonic: cache directory is %s\n", tectonic_cache_dir);
    }
    else
    {
      fprintf(stderr, "tectonic provider: malformed cache path\n");
      return NULL;
    }
  }

  char *ptr = stpcpy(g_path_buf, tectonic_cache_dir);
  for (const char **s = suffixes; *s; s++)
    ptr = stpcpy(ptr, *s);
  return g_path_buf;
}

#define tectonic_user_cache_path(...) \
  tectonic_user_cache_path_impl(      \
      (const char *[]){__VA_ARGS__ __VA_OPT__(, ) NULL})

/* -------------------------------------------------------------------------- */
/* Shell Quoting & Command Building (Allocation-Free)                         */
/* -------------------------------------------------------------------------- */

static void shell_quote(char *buf, const char *name)
{
  *buf++ = '\'';
  while (*name)
  {
    if (*name == '\'')
    {
      *buf++ = '\\';
      *buf++ = '\'';
      *buf++ = '\'';
    }
    else
    {
      *buf++ = *name;
    }
    name++;
  }
  *buf++ = '\'';
  *buf = '\0';
}

static const char *build_cat_cmd(const char *name, bool to_stdout)
{
  static char buf[TECTONIC_CMD_MAX];
  char *p = stpcpy(buf, to_stdout ? "tectonic -X bundle cat "
                                  : ">/dev/null tectonic -X bundle cat ");
  shell_quote(p, name);
  return buf;
}

/* -------------------------------------------------------------------------- */
/* Tectonic 0.16 Support                                                      */
/* -------------------------------------------------------------------------- */

static void tt16_add_dir(const char *name)
{
  size_t name_len = strlen(name) + 1;
  size_t new_len = tt_dirs.len + name_len;
  dynbuf_ensure_capacity(&tt_dirs, new_len);
  memcpy(tt_dirs.data + tt_dirs.len, name, name_len);
  tt_dirs.len = new_len;
}

static void tt16_add_to_index(const char *buffer, size_t len)
{
  size_t new_cap = tt_index.len + len;
  dynbuf_ensure_capacity(&tt_index, new_cap);
  for (size_t i = 0; i < len; i++)
  {
    if (tt_index_skip)
    {
      if (buffer[i] == '\n')
        tt_index_skip = false;
    }
    else
    {
      if (buffer[i] == ' ' || buffer[i] == '\n')
      {
        tt_index_skip = true;
        tt_index.data[tt_index.len++] = '\0';
      }
      else
      {
        tt_index.data[tt_index.len++] = buffer[i];
      }
    }
  }
}

static void tt16_load_indexes(void)
{
  const char *data_dir = tectonic_user_cache_path("/data/");
  if (!data_dir)
    return;

  DIR *dirp = opendir(data_dir);
  if (!dirp)
    return;

  struct dirent *entry;
  while ((entry = readdir(dirp)) != NULL)
  {
    if (entry->d_type == DT_DIR)
    {
      tt16_add_dir(entry->d_name);
    }
    else if (entry->d_type == DT_REG)
    {
      size_t name_len = strlen(entry->d_name);
      if (name_len >= 6 && strcmp(entry->d_name + name_len - 6, ".index") == 0)
      {
        const char *idx_path =
            tectonic_user_cache_path("/data/", entry->d_name);
        FILE *f = fopen(idx_path, "r");
        if (f)
        {
          char buf[READ_BUF_SIZE];
          ssize_t n;
          while ((n = fread(buf, 1, READ_BUF_SIZE, f)) > 0)
            tt16_add_to_index(buf, n);
          fclose(f);
        }
        else
        {
          fprintf(stderr, "tectonic provider: cannot read index file %s\n",
                  idx_path);
        }
      }
    }
  }
  closedir(dirp);
}

static bool tt16_list_tectonic_files(void)
{
  tt16_load_indexes();
  if (tt_index.len == 0)
  {
    fprintf(stderr,
            "Cannot find Tectonic bundle(s) manifests. Trying to initialize "
            "tectonic now.\n");
    system("tectonic -X bundle cat SHA256SUM >/dev/null");
    tt16_load_indexes();
  }
  if (tt_index.len == 0)
    return false;

  build_hash_table();
  return true;
}

static const char *tt16_get_file_path(const char *name)
{
  static char result_path[TECTONIC_PATH_MAX];
  for (const char *dir = tt_dirs.data, *end = dir + tt_dirs.len; dir < end;
       dir += strlen(dir) + 1)
  {
    const char *path = tectonic_user_cache_path("/data/", dir, "/", name);
    if (access(path, R_OK) != 0)
      continue;
    strcpy(result_path, path);
    return result_path;
  }

  fprintf(stderr,
          "tectonic provider: %s missing, trying to fetch with tectonic\n",
          name);

  int retcode = system(build_cat_cmd(name, false));
  if (retcode != 0)
    fprintf(stderr, "tectonic provider: command returned code %d\n", retcode);

  for (const char *dir = tt_dirs.data, *end = dir + tt_dirs.len; dir < end;
       dir += strlen(dir) + 1)
  {
    const char *path = tectonic_user_cache_path("/data/", dir, "/", name);
    if (access(path, R_OK) != 0)
      continue;
    strcpy(result_path, path);
    fprintf(stderr, "tectonic provider: found %s\n", result_path);
    return result_path;
  }

  do_abortf("tectonic provider: cannot load %s, skipping\n", name);
  return NULL;
}

/* -------------------------------------------------------------------------- */
/* Tectonic 0.15 Support                                                      */
/* -------------------------------------------------------------------------- */

static void tt15_append_buffer(const char *buffer, size_t len)
{
  size_t new_len = tt_index.len + len;
  dynbuf_ensure_capacity(&tt_index, new_len);
  char *data = tt_index.data + tt_index.len;
  for (size_t i = 0; i < len; i++)
  {
    if (buffer[i] == '\n')
      data[i] = '\0';
    else
      data[i] = buffer[i];
  }
  tt_index.len += len;
}

static bool tt15_check_cache_validity(void)
{
  const char *dir = cache_path("tectonic", "SHA256SUM");
  if (!dir)
    return false;

  FILE *f = fopen(dir, "rb");
  if (!f)
    return false;

  char b1[READ_HASH_SIZE];
  size_t r1 = fread(b1, 1, READ_HASH_SIZE, f);
  fclose(f);

  FILE *p = popen("tectonic -X bundle cat SHA256SUM", "r");
  if (!p)
    return false;

  char b2[READ_HASH_SIZE];
  size_t r2 = fread(b2, 1, READ_HASH_SIZE, p);
  pclose(p);

  return (r1 == r2) && (memcmp(b1, b2, r1) == 0);
}

static void tt15_prepare_cache(void)
{
  if (tt15_check_cache_validity())
    return;

  const char *path = cache_path("tectonic", NULL);
  if (!path)
    return;

  DIR *dir = opendir(path);
  if (!dir)
  {
    perror("opendir");
    return;
  }

  struct dirent *entry;
  char filePath[TECTONIC_PATH_MAX];
  while ((entry = readdir(dir)) != NULL)
  {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    snprintf(filePath, sizeof(filePath), "%s/%s", path, entry->d_name);
    if (unlink(filePath) != 0)
      perror("unlink");
  }
  closedir(dir);

  FILE *f = tectonic_get_file("SHA256SUM");
  if (f)
    fclose(f);
}

static bool tt15_list_tectonic_files(void)
{
  FILE *f = popen("tectonic -X bundle search", "r");
  if (!f)
    do_abortf("Failed to execute 'tectonic -X bundle search'");

  char buffer[READ_HASH_SIZE];
  ssize_t n;
  while ((n = fread(buffer, 1, READ_HASH_SIZE, f)) > 0)
    tt15_append_buffer(buffer, n);

  if (ferror(f))
    perror("fread/popen");
  pclose(f);

  build_hash_table();
  tt15_prepare_cache();
  return tt_index.len > 0;
}

static FILE *tt15_cat(const char *name)
{
  return popen(build_cat_cmd(name, true), "r");
}

static const char *tt15_get_file_path(const char *name)
{
  const char *cached = cache_path("tectonic", name);
  if (access(cached, R_OK) == 0)
    return cached;

  FILE *f = fopen(cached, "wb");
  if (!f)
    return NULL;

  FILE *p = tt15_cat(name);
  if (!p)
  {
    fclose(f);
    return NULL;
  }

  char buffer[READ_BUF_SIZE];
  ssize_t n;
  while ((n = fread(buffer, 1, READ_BUF_SIZE, p)) > 0)
  {
    if (fwrite(buffer, 1, n, f) != (size_t)n)
      break;
  }

  bool error = (n != 0) || ferror(p) || ferror(f);
  pclose(p);
  fclose(f);

  if (error)
  {
    unlink(cached);
    return NULL;
  }
  return cached;
}

/* -------------------------------------------------------------------------- */
/* Version Dispatch & Public API                                              */
/* -------------------------------------------------------------------------- */

static bool list_tectonic_files(void)
{
  static bool loaded = false, result = false;
  if (loaded)
    return false;
  loaded = true;

  if (tt16_list_tectonic_files())
    return (result = true);
  tt_is_v15 = true;
  if (tt15_list_tectonic_files())
    return (result = true);

  return false;
}

const char *tectonic_get_file_path(const char *name)
{
  if (!tectonic_has_file(name))
    return NULL;
  else if (tt_is_v15)
    return tt15_get_file_path(name);
  else
    return tt16_get_file_path(name);
}

FILE *tectonic_get_file(const char *name)
{
  const char *path = tectonic_get_file_path(name);
  return path ? fopen(path, "rb") : NULL;
}

void tectonic_record_version(FILE *fr)
{
  FILE *fh = tectonic_get_file("SHA256SUM");
  if (!fh)
  {
    fwrite("!", 1, 1, fr);
    return;
  }
  char buffer[READ_HASH_SIZE];
  ssize_t n;
  while ((n = fread(buffer, 1, READ_HASH_SIZE, fh)) > 0)
  {
    if (fwrite(buffer, 1, n, fr) != (size_t)n)
      break;
  }
  fclose(fh);
}

bool tectonic_check_version(FILE *fr)
{
  FILE *fh = tectonic_get_file("SHA256SUM");
  if (!fh)
  {
    char c;
    return (fread(&c, 1, 1, fr) == 1 && c == '!');
  }
  char b1[READ_HASH_SIZE], b2[READ_HASH_SIZE];
  ssize_t n;
  bool valid = true;
  while ((n = fread(b1, 1, READ_HASH_SIZE, fh)) > 0)
  {
    if ((fread(b2, 1, n, fr) == (size_t)n) && (memcmp(b1, b2, n) == 0))
      continue;
    valid = false;
    break;
  }
  valid = valid && (feof(fh) && !ferror(fh));
  fclose(fh);
  return valid;
}

bool tectonic_has_file(const char *name)
{
  list_tectonic_files();
  int index = lookup_entry_index(name);
  return entries[index] != -1;
}

bool tectonic_available(void)
{
  return tectonic_has_file("SHA256SUM");
}
