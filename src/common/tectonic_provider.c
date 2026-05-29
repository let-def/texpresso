#include "common.h"
#include "providers.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int *entries;
int entries_pow = 1;

char *tt_index = NULL;
size_t tt_index_len = 0, tt_index_cap = 1;
bool tt_index_skip = 0;
bool tt_is_v15 = 0;

static unsigned long sdbm_hash(const void *p)
{
  unsigned long hash = 0;
  int c;

  for (const unsigned char *str = p; (c = *str); str++)
    hash = c + (hash << 6) + (hash << 16) - hash;

  return hash * 2654435761;
}

int lookup_entry_index(const char *name)
{
  unsigned long hash = sdbm_hash(name);
  int mask = (1 << entries_pow) - 1;
  int index = hash & mask;

  while (1)
  {
    int offset = entries[index];
    if (offset == -1)
      return index;
    if (strcmp(name, tt_index + offset) == 0)
      return index;
    index = (index + 1) & mask;
  }
}

static const char *tectonic_user_cache_path(const char *suffixes[])
{
  static char dir[4096] = {0,}, *ptr = NULL;
  static bool init = 0;

  if (!init)
  {
    init = 1;

    FILE *p = popen("tectonic -X show user-cache-dir", "r");

    // Tectonic not found
    if (!p)
    {
      fprintf(stderr, "tectonic: not found\n");
      return 0;
    }

    size_t n = fread(dir, 1, sizeof(dir) - 1, p);

    if (n > 0 && dir[n-1] == '\n')
    {
      n--;
      dir[n] = 0;
      ptr = dir + n;
      fprintf(stderr, "tectonic: cache directory is %s\n", dir);
    }
    else
      fprintf(stderr, "tectonic provider: malformed cache path: \"%s\"\n", dir);
  }

  if (!ptr)
    return NULL;

  if (suffixes)
  {
    char *p = ptr;
    while (*suffixes)
    {
      p = stpcpy(p, *suffixes);
      suffixes++;
    }
  }
  return dir;
}
#define tectonic_user_cache_path(...) tectonic_user_cache_path((const char *[]){__VA_ARGS__ __VA_OPT__(,) NULL})

/* Tectonic 0.16 support */

char *tt_dirs = NULL;
size_t tt_dirs_len = 0, tt_dirs_cap = 1;

static void tt16_add_dir(char *name)
{
  size_t name_len = strlen(name) + 1;
  size_t new_len = tt_dirs_len + name_len;
  if (new_len >= tt_dirs_cap)
  {
    while (new_len >= tt_dirs_cap)
      tt_dirs_cap *= 2;
    tt_dirs = realloc(tt_dirs, tt_dirs_cap);
    if (tt_dirs == NULL)
      do_abortf("tectonic provider: cannot allocate buffer to store bundle directory");
  }

  memmove(tt_dirs + tt_dirs_len, name, name_len);
  tt_dirs_len = new_len;
}

static void tt16_add_to_index(char *buffer, int len)
{
  size_t new_cap = tt_index_len + len;
  if (new_cap >= tt_index_cap)
  {
    while (new_cap >= tt_index_cap)
      tt_index_cap *= 2;
    tt_index = realloc(tt_index, tt_index_cap);
    if (tt_index == NULL)
      do_abortf("tectonic provider: cannot allocate buffer to read bundle index");
  }

  for (int i = 0; i < len; i++)
  {
    if (tt_index_skip)
    {
      if (buffer[i] == '\n')
        tt_index_skip = 0;
    }
    else
    {
      if (buffer[i] == ' ' || buffer[i] == '\n')
      {
        tt_index_skip = 1;
        tt_index[tt_index_len++] = '\0';
      }
      else
        tt_index[tt_index_len++] = buffer[i];
    }
  }
}

static void tt16_load_index_file(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
  {
    fprintf(stderr, "tectonic provider: cannot read index file %s\n", path);
    return;
  }

  bool skip = false;
  char buffer[4096];
  ssize_t read;

  while ((read = fread(buffer, 1, 4096, f)) > 0)
    tt16_add_to_index(buffer, read);

  if (ferror(f))
    perror("fread");

  fclose(f);
}

static void tt16_load_indexes(void)
{
  const char *data_dir = tectonic_user_cache_path("/data/");
  DIR *dirp = NULL;

  if (!data_dir || !(dirp = opendir(data_dir)))
    return;

  // We found the data directory, load all .index files
  struct dirent *entry;
  const char *suffix = ".index";
  size_t suffix_len = strlen(suffix);

  while ((entry = readdir(dirp)) != NULL)
  {
    if (entry->d_type == DT_DIR)
      tt16_add_dir(entry->d_name);
    else if (entry->d_type == DT_REG)
    {
      size_t name_len = strlen(entry->d_name);
      if (name_len >= suffix_len &&
          strcmp(entry->d_name + (name_len - suffix_len), suffix) == 0)
        tt16_load_index_file(tectonic_user_cache_path("/data/", entry->d_name));
    }
  }

  closedir(dirp);
}

static bool tt16_list_tectonic_files(void)
{
  tt16_load_indexes();

  if (tt_index_len == 0)
  {
    fprintf(stderr,
            "Cannot find Tectonic bundle(s) manifests. Trying to initialize "
            "tectonic now.\n");
    system("tectonic -X bundle cat SHA256SUM");
    tt16_load_indexes();
  }
  if (tt_index_len == 0)
    return false;

  // Count number of entries
  int count = 0;
  for (int i = 0; i < tt_index_len; i++)
    if (tt_index[i] == '\0')
      count += 1;

  // Allocate hashtable for indexing lines
  int min_cap = count * 4 / 3;
  while ((1 << entries_pow) <= min_cap)
    entries_pow++;
  entries = malloc((1 << entries_pow) * sizeof(*entries));
  if (entries == NULL)
    do_abortf("Cannot allocate table for indexing Tectonic bundle.");
  for (int i = 0; i < (1 << entries_pow); i++)
    entries[i] = -1;

  // Populate table
  for (int i = 0, j = 0; j < tt_index_len; j++)
  {
    if (tt_index[j] != '\0')
      continue;

    if (i != j)
    {
      int index = lookup_entry_index(tt_index + i);
      if (entries[index] == -1)
        entries[index] = i;
      else
        fprintf(stderr, "tectonic bundle: duplicate entry (%s)\n",
                tt_index + i);
    }
    i = j + 1;
  }

  fprintf(stderr, "tectonic bundle: indexing succeeded (%d entries)\n", count);
  return true;
}

static const char *tt16_get_file_path(const char *name)
{
  for (const char *dir = tt_dirs; dir < tt_dirs + tt_dirs_len; dir = dir + strlen(dir) + 1)
  {
    const char *path = tectonic_user_cache_path("/data/", dir, "/", name);
    if (access(path, R_OK) == 0)
      return path;
  }

  fprintf(stderr, "tectonic provider: %s missing, trying to fetch with tectonic\n", name);

  {
    char command[1024] = ">/dev/null tectonic -X bundle cat ";
    char *p = command;
    const char *pname = name;

    while (*p)
      p++;

    *p++ = '\'';
    while (*pname)
    {
      if ((*p++ = *pname++) != '\'')
        continue;
      *p++ = '\\';
      *p++ = '\'';
      *p++ = '\'';
    }
    *p++ = '\'';
    *p = '\0';

    int retcode = system(command);
    if (retcode != 0)
      fprintf(stderr, "tectonic provider: \"%s\" returned code %d\n", command,
              retcode);
  }

  for (const char *dir = tt_dirs; dir < tt_dirs + tt_dirs_len; dir = dir + strlen(dir) + 1)
  {
    const char *path = tectonic_user_cache_path("/data/", dir, "/", name);
    if (access(path, R_OK) == 0)
    {
      fprintf(stderr, "tectonic provider: found %s\n", path);
      return path;
    }
  }

  do_abortf("tectonic provider: cannot load %s, skipping\n", name);
  return NULL;
}

/* Tectonic old versions support */

static void tt15_append_buffer(char *buffer, int len)
{
  size_t new_len = tt_index_len + len;
  if (new_len > tt_index_cap)
  {
    while (new_len > tt_index_cap)
      tt_index_cap *= 2;
    tt_index = realloc(tt_index, tt_index_cap);
    if (tt_index == NULL)
      do_abortf("Cannot allocate buffer to read Tectonic bundle");
  }
  memmove(tt_index + tt_index_len, buffer, len);
  tt_index_len += len;
}

static bool tt15_check_cache_validity(void)
{
  const char *dir = cache_path("tectonic", "SHA256SUM");

  // Give up if there is no cache directory
  if (!dir)
    return 0;

  FILE *f;

  // Check version of our cache
  f = fopen(dir, "rb");
  // No SHA256SUM: cache is either uninitialized or damaged
  if (!f)
    return 0;

  char b1[4096];
  int r1 = fread(b1, 1, 4096, f);
  fclose(f);

  // Compare tectonic SHA256SUM and ours
  FILE *p = popen("tectonic -X bundle cat SHA256SUM", "r");
  // No tectonic: we cannot check cache :/
  if (!p)
    return 0;

  char b2[4096];
  int r2 = fread(b2, 1, 4096, p);
  b2[r2] = 0;
  pclose(p);

  return (r1 == r2) && (memcmp(b1, b2, r1) == 0);
}

static void tt15_prepare_cache(void)
{
  if (tt15_check_cache_validity())
    return;

  // Base cache directory
  const char *path = cache_path("tectonic", NULL);

  DIR *dir;
  struct dirent *entry;
  char filePath[1024];

  if ((dir = opendir(path)) == NULL)
  {
    perror("opendir");
    return;
  }

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
  if (f) fclose(f);
}

static bool tt15_list_tectonic_files(void)
{
  // Read the list of all files
  FILE *f = popen("tectonic -X bundle search", "r");
  if (f == NULL)
    do_abortf("Failed to execute 'tectonic -X bundle search'");

  char buffer[4096];
  ssize_t read;
  while ((read = fread(buffer, 1, 4096, f)) > 0)
    tt15_append_buffer(buffer, read);

  if (ferror(f))
    perror("fread/popen");

  pclose(f);

  // Count number of lines
  int count = 0;
  for (int i = 0; i < tt_index_len; i++)
    if (tt_index[i] == '\n')
      count += 1;

  // Allocate hashtable for indexing lines
  int min_cap = count * 4 / 3;
  while ((1 << entries_pow) <= min_cap)
    entries_pow++;
  entries = malloc((1 << entries_pow) * sizeof(*entries));
  if (entries == NULL)
    do_abortf("Cannot allocate table for indexing Tectonic bundle.");
  for (int i = 0; i < (1 << entries_pow); i++)
    entries[i] = -1;

  // Populate table
  for (int i = 0, j = 0; j < tt_index_len; j++)
  {
    if (tt_index[j] == '\n')
    {
      tt_index[j] = '\0';
      int index = lookup_entry_index(tt_index + i);
      if (entries[index] == -1)
        entries[index] = i;
      else
        fprintf(stderr, "tectonic bundle: duplicate entry (%s)\n", tt_index + i);
      i = j + 1;
    }
  }

  fprintf(stderr, "tectonic bundle: indexing succeeded (%d entries)\n", count);
  tt15_prepare_cache();

  return tt_index_len > 0;
}

static FILE *tt15_cat(const char *name)
{
  if (!tectonic_has_file(name))
    return NULL;
  char buffer[1024] = "tectonic -X bundle cat ", *p = buffer;
  while (*p)
    p++;

  *p++ = '\'';
  while (*name)
  {
    if ((*p++ = *name++) != '\'')
      continue;
    *p++ = '\\';
    *p++ = '\'';
    *p++ = '\'';
  }
  *p++ = '\'';
  *p = '\0';

  return popen(buffer, "r");
}

static const char *tt15_get_file_path(const char *name)
{
  if (!tectonic_has_file(name))
    return NULL;

  const char *cached = cache_path("tectonic", name);
  if (access(cached, R_OK) == 0)
    return cached;

  FILE *f = fopen(cached, "wb");
  if (!f)
    return NULL;

  FILE *p = tt15_cat(name);
  if (!p)
    return NULL;

  char buffer[4096];
  int read;
  while ((read = fread(buffer, 1, 4096, p)) > 0)
  {
    if (fwrite(buffer, 1, read, f) != read)
      break;
  }

  bool error = read != 0 || ferror(p) || ferror(f);
  pclose(p);
  fclose(f);
  if (error)
  {
    unlink(cached);
    return NULL;
  }

  return cached;
}

/* Version dispatch functions */

const char *tectonic_get_file_path(const char *name)
{
  if (!tectonic_has_file(name))
    return NULL;

  return tt_is_v15 ? tt15_get_file_path(name) : tt16_get_file_path(name);
}

static void list_tectonic_files(void)
{
  static int loaded = 0;
  if (loaded)
    return;
  loaded = 1;

  if (tt16_list_tectonic_files())
    return;

  tt_is_v15 = 1;
  if (tt15_list_tectonic_files())
    return;

  // No manifest... Tectonic was not initialized?
  fprintf(stderr,
          "Cannot find Tectonic bundle(s) manifests.\n"
          "Please compile a document with tectonic before launching "
          "TeXpresso.\n");
  exit(1);
}

/* Generic functions */

FILE *tectonic_get_file(const char *name)
{
  const char *path = tectonic_get_file_path(name);
  if (path)
    return fopen(path, "rb");
  else
    return NULL;
}

void tectonic_record_version(FILE *fr)
{
  FILE *fh = tectonic_get_file("SHA256SUM");
  if (!fh)
  {
    fwrite("!", 1, 1, fr);
    return;
  }

  char buffer[4096];
  int read;
  while ((read = fread(buffer, 1, 4096, fh)) > 0)
  {
    if (fwrite(buffer, 1, read, fr) != read)
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

  char b1[4096], b2[4096];
  int read, valid = 1;
  while ((read = fread(b1, 1, 4096, fh)) > 0)
  {
    if ((fread(b2, 1, read, fr) == read) && (memcmp(b1, b2, read) == 0))
      continue;
    valid = 0;
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
  int offset = entries[index];
  return offset != -1;
}

bool tectonic_available(void)
{
  return tectonic_has_file("SHA256SUM");
}
